// OCR frame batch pipeline: the per-sample codec, batch sizing, batch
// assembly, and the property that the reduced OCR output does not depend on
// how the sample plan was batched or in which order batches finished.

#include "../src/ocr_generation/ocr_generation_internal.hpp"

#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_batch_policy.hpp"
#include "svp/vision/ocr_frame_batch_reduction.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_sample_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SVP_VISION_TEST_FIXTURES_DIR
#error "SVP_VISION_TEST_FIXTURES_DIR must name packages/svp-vision/tests/fixtures"
#endif

namespace {

namespace vision = svp::vision;
namespace internal = svp::vision::ocr_generation_internal;
using vision::OcrSampleDetections;
using vision::OcrSampleStatus;
using Batches = std::vector<std::vector<OcrSampleDetections>>;

// Recorded payload of a real heavy-frame run: PP-OCRv6 medium on four of the
// Gator frames with the most text boxes (one text-dense scene, 576.8-580.9 s,
// 603 detections), captured through ocr.frame_batch with
// SVP_OCR_TASK_TEST_FIXTURE_OUT (per-frame detection counts match the
// builder's provenance for the same timestamps). Text repeated across the
// frames exercises the cross-frame clustering in reconcile_detections.
const std::filesystem::path kHeavyFixture =
    std::filesystem::path(SVP_VISION_TEST_FIXTURES_DIR) / "ocr_frame_detections_heavy.jsonl";

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Exception>
void require_throws(const std::function<void()>& action, const std::string& message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  }
  require(false, message);
}

OcrSampleDetections ok_sample(std::uint64_t ordinal, std::int64_t timestamp_us,
                              std::vector<vision::OcrTextDetection> detections = {}) {
  OcrSampleDetections record;
  record.sample_ordinal = ordinal;
  record.timestamp_us = timestamp_us;
  record.frame_width = 1920;
  record.frame_height = 1080;
  record.detections = std::move(detections);
  return record;
}

OcrSampleDetections missed_sample(std::uint64_t ordinal, std::int64_t timestamp_us) {
  OcrSampleDetections record;
  record.sample_ordinal = ordinal;
  record.timestamp_us = timestamp_us;
  record.status = OcrSampleStatus::decode_missed;
  record.error = "short read: got 0/6220800 bytes (ffmpeg exit 0)";
  return record;
}

// --- codec -----------------------------------------------------------------

std::vector<OcrSampleDetections> golden_records() {
  OcrSampleDetections failed = ok_sample(2, 2'000'000);
  failed.status = OcrSampleStatus::ocr_failed;
  failed.error = "bad \"input\"";
  return {
      ok_sample(0, 0,
                {{.text = "SALE $4,997", .confidence = 0.875, .bbox_left = 10,
                  .bbox_top = 20, .bbox_right = 300, .bbox_bottom = 60},
                 {.text = "\xC3\x9Cn\xC3\xAF", .confidence = 0.1 + 0.2, .bbox_left = 1,
                  .bbox_top = 2, .bbox_right = 3, .bbox_bottom = 4}}),
      missed_sample(1, 1'000'000),
      failed,
      ok_sample(3, 3'000'000),
  };
}

void test_codec_golden_bytes() {
  const std::string expected =
      R"({"detections":[{"bbox":[10,20,300,60],"confidence":0.875,"text":"SALE $4,997"},)"
      "{\"bbox\":[1,2,3,4],\"confidence\":0.30000000000000004,\"text\":\"\xC3\x9Cn\xC3\xAF\"}],"
      R"("frame_height":1080,"frame_width":1920,"sample_ordinal":0,"status":"ok","timestamp_us":0})"
      "\n"
      R"j({"error":"short read: got 0/6220800 bytes (ffmpeg exit 0)","sample_ordinal":1,)j"
      R"("status":"decode_missed","timestamp_us":1000000})"
      "\n"
      R"({"error":"bad \"input\"","frame_height":1080,"frame_width":1920,"sample_ordinal":2,)"
      R"("status":"ocr_failed","timestamp_us":2000000})"
      "\n"
      R"({"detections":[],"frame_height":1080,"frame_width":1920,"sample_ordinal":3,)"
      R"("status":"ok","timestamp_us":3000000})"
      "\n";
  const std::vector<OcrSampleDetections> records = golden_records();
  const std::string bytes = vision::encode_ocr_sample_detections_jsonl(records);
  require(bytes == expected, "codec golden bytes:\n" + bytes);
  // Exact round trip, including a confidence that is not a short decimal.
  require(vision::decode_ocr_sample_detections_jsonl(bytes) == records, "codec round trip");
  require(vision::encode_ocr_sample_detections_jsonl(
              vision::decode_ocr_sample_detections_jsonl(bytes)) == bytes,
          "codec re-encode is byte-stable");
  require(vision::decode_ocr_sample_detections_jsonl("").empty(), "empty payload");
}

void test_codec_rejects() {
  const std::string valid_ok =
      R"({"detections":[],"frame_height":1080,"frame_width":1920,"sample_ordinal":3,)"
      R"("status":"ok","timestamp_us":3000000})";
  require(vision::decode_ocr_sample_detections_jsonl(valid_ok + "\n").size() == 1,
          "valid line accepted");
  const std::vector<std::pair<std::string, std::string>> bad_payloads = {
      {"missing final newline", valid_ok},
      {"insignificant whitespace",
       R"({"detections": [],"frame_height":1080,"frame_width":1920,"sample_ordinal":3,)"
       R"("status":"ok","timestamp_us":3000000})"
       "\n"},
      {"unsorted keys",
       R"({"frame_height":1080,"detections":[],"frame_width":1920,"sample_ordinal":3,)"
       R"("status":"ok","timestamp_us":3000000})"
       "\n"},
      {"unknown field",
       R"({"detections":[],"extra":1,"frame_height":1080,"frame_width":1920,)"
       R"("sample_ordinal":3,"status":"ok","timestamp_us":3000000})"
       "\n"},
      {"integer confidence",
       R"({"detections":[{"bbox":[1,2,3,4],"confidence":1,"text":"A"}],"frame_height":1080,)"
       R"("frame_width":1920,"sample_ordinal":3,"status":"ok","timestamp_us":3000000})"
       "\n"},
      {"ok with error",
       R"({"detections":[],"error":"x","frame_height":1080,"frame_width":1920,)"
       R"("sample_ordinal":3,"status":"ok","timestamp_us":3000000})"
       "\n"},
      {"decode miss with frame size",
       R"({"error":"x","frame_height":1080,"frame_width":1920,"sample_ordinal":1,)"
       R"("status":"decode_missed","timestamp_us":1000000})"
       "\n"},
      {"unknown status",
       R"({"error":"x","sample_ordinal":1,"status":"lost","timestamp_us":1000000})"
       "\n"},
      {"negative ordinal",
       R"({"error":"x","sample_ordinal":-1,"status":"decode_missed","timestamp_us":1})"
       "\n"},
      {"short bbox",
       R"({"detections":[{"bbox":[1,2,3],"confidence":1.0,"text":"A"}],"frame_height":1080,)"
       R"("frame_width":1920,"sample_ordinal":3,"status":"ok","timestamp_us":3000000})"
       "\n"},
      {"deep nesting", std::string(64, '[') + std::string(64, ']') + "\n"},
      {"not JSON", "{\n"},
  };
  for (const auto& [name, payload] : bad_payloads) {
    require_throws<vision::OcrFrameDetectionsCodecError>(
        [&] { (void)vision::decode_ocr_sample_detections_jsonl(payload); },
        "codec must reject: " + name);
  }

  OcrSampleDetections nan_confidence =
      ok_sample(0, 0, {{.text = "A", .confidence = std::numeric_limits<double>::quiet_NaN()}});
  require_throws<vision::OcrFrameDetectionsCodecError>(
      [&] { (void)vision::ocr_sample_detections_to_json(nan_confidence); },
      "codec must reject a NaN confidence");
  OcrSampleDetections ok_with_error = ok_sample(0, 0);
  ok_with_error.error = "x";
  require_throws<vision::OcrFrameDetectionsCodecError>(
      [&] { (void)vision::ocr_sample_detections_to_json(ok_with_error); },
      "codec must reject an ok sample with an error");
}

// --- batch policy ----------------------------------------------------------

void test_batch_policy() {
  require(vision::ocr_batch_sample_count({.target_task_seconds = 3.0,
                                          .estimated_seconds_per_sample = 1.0}) == 3,
          "three one-second samples fit a three-second batch");
  require(vision::ocr_batch_sample_count({.target_task_seconds = 0.1,
                                          .estimated_seconds_per_sample = 2.0}) == 1,
          "a batch holds at least one sample");
  const vision::OcrBatchPolicy defaults;
  require(vision::ocr_batch_sample_count(defaults) ==
              static_cast<std::uint64_t>(std::floor(defaults.target_task_seconds /
                                                    defaults.estimated_seconds_per_sample)),
          "default batch size follows the policy");

  const auto batches = vision::partition_ocr_samples(
      8, {.target_task_seconds = 3.0, .estimated_seconds_per_sample = 1.0});
  require(batches == std::vector<vision::OcrSampleBatch>{{0, 3}, {3, 3}, {6, 2}},
          "partition is consecutive and covers every sample once");
  require(vision::partition_ocr_samples(0, defaults).empty(), "no samples, no batches");

  for (const vision::OcrBatchPolicy bad :
       {vision::OcrBatchPolicy{.target_task_seconds = 0.0},
        vision::OcrBatchPolicy{.estimated_seconds_per_sample = -1.0},
        vision::OcrBatchPolicy{.target_task_seconds = std::nan("")}}) {
    require_throws<std::invalid_argument>(
        [&] { (void)vision::partition_ocr_samples(4, bad); }, "invalid policy rejected");
  }
}

// --- batch assembly --------------------------------------------------------

vision::OcrSamplePlan plan_for(const std::vector<std::int64_t>& timestamps) {
  vision::OcrTemporalSamplingResult sampling;
  sampling.timestamps_us = timestamps;
  sampling.sample_count = static_cast<int>(timestamps.size());
  return vision::make_ocr_sample_plan(sampling, 1920, 1080);
}

void test_assembly_rejects_bad_coverage() {
  const vision::OcrSamplePlan plan = plan_for({0, 1'000'000, 2'000'000, 3'000'000});
  const auto assemble = [&](Batches batches) {
    return vision::assemble_ocr_frame_batches(plan, std::move(batches));
  };
  const std::vector<OcrSampleDetections> ordered =
      assemble({{ok_sample(2, 2'000'000), ok_sample(3, 3'000'000)},
                {missed_sample(1, 1'000'000), ok_sample(0, 0)}});
  require(ordered.size() == 4, "all samples assembled");
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    require(ordered[index].sample_ordinal == index, "assembled in ordinal order");
  }

  const std::vector<std::pair<std::string, Batches>> bad = {
      {"gap", {{ok_sample(0, 0), ok_sample(1, 1'000'000)}, {ok_sample(3, 3'000'000)}}},
      {"duplicate",
       {{ok_sample(0, 0), ok_sample(1, 1'000'000), ok_sample(2, 2'000'000)},
        {ok_sample(2, 2'000'000), ok_sample(3, 3'000'000)}}},
      {"ordinal outside the plan",
       {{ok_sample(0, 0), ok_sample(1, 1'000'000), ok_sample(2, 2'000'000),
         ok_sample(3, 3'000'000), ok_sample(4, 4'000'000)}}},
      {"timestamp differs from the plan",
       {{ok_sample(0, 0), ok_sample(1, 1'000'001), ok_sample(2, 2'000'000),
         ok_sample(3, 3'000'000)}}},
      {"no batches", {}},
  };
  for (const auto& [name, batches] : bad) {
    require_throws<vision::OcrFrameBatchReductionError>(
        [&] { (void)assemble(batches); }, "assembly must reject: " + name);
  }
  OcrSampleDetections wrong_size = ok_sample(0, 0);
  wrong_size.frame_width = 1280;
  require_throws<vision::OcrFrameBatchReductionError>(
      [&] {
        (void)assemble({{wrong_size, ok_sample(1, 1'000'000), ok_sample(2, 2'000'000),
                         ok_sample(3, 3'000'000)}});
      },
      "assembly must reject a frame decoded at another size");
}

// --- partition independence over recorded detections ----------------------

std::vector<OcrSampleDetections> load_fixture() {
  std::ifstream input(kHeavyFixture, std::ios::binary);
  require(input.good(), "cannot read " + kHeavyFixture.string());
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  return vision::decode_ocr_sample_detections_jsonl(bytes);
}

// The fixture's samples with a decode miss inserted between the first two,
// renumbered, so frame registration must skip a sample.
std::vector<OcrSampleDetections> samples_with_a_miss(std::vector<OcrSampleDetections> recorded) {
  require(recorded.size() >= 2, "fixture has at least two samples");
  const std::int64_t between = recorded[0].timestamp_us +
      (recorded[1].timestamp_us - recorded[0].timestamp_us) / 2;
  recorded.insert(recorded.begin() + 1, missed_sample(0, between));
  for (std::size_t index = 0; index < recorded.size(); ++index) {
    recorded[index].sample_ordinal = index;
  }
  return recorded;
}

struct ReducedText {
  std::string bytes;
  std::size_t observations = 0;
  std::size_t multi_frame_observations = 0;
};

// Everything the reducer derives from sample records: frame registration
// (catalog IDs, keyframe), per-frame diagnostics, and the reconciled region,
// observation, and numeric records with their sequential IDs.
ReducedText reduce(const vision::OcrSamplePlan& plan, Batches batches) {
  const std::vector<OcrSampleDetections> ordered =
      vision::assemble_ocr_frame_batches(plan, std::move(batches));

  // Frames registered by earlier stages come first, as color frames do.
  vision::FrameCatalog catalog;
  for (const std::int64_t timestamp : {500'000, 1'500'000}) {
    (void)catalog.register_frame(timestamp, vision::kColorFramePurpose);
  }
  const auto frames = internal::register_ocr_sample_frames(ordered, &catalog);
  internal::CollectedOcrFrames collected;
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    if (frames[index]) internal::collect_ocr_sample(ordered[index], *frames[index], collected);
  }
  const auto reconciled =
      internal::reconcile_detections(collected.detections, collected.processed_frame_count);
  vision::OcrGenerationOptions options;
  options.canonical_raster_width = 640;
  options.canonical_raster_height = 360;
  vision::OcrGenerationResult result;
  internal::emit_reconciled_records(options, reconciled, result);

  ReducedText reduced;
  for (const auto& entry : catalog.entries()) {
    reduced.bytes += entry.frame_id + " " + std::to_string(entry.timestamp_us) +
                     (entry.keyframe ? " key" : "") + "\n";
  }
  for (const auto& diagnostic : collected.frame_diagnostics) {
    reduced.bytes += diagnostic.dump() + "\n";
  }
  for (const auto& region : result.text_regions) {
    reduced.bytes += vision::text_region_to_json(region).dump() + "\n";
  }
  for (const auto& observation : result.text_observations) {
    reduced.bytes += vision::text_observation_to_json(observation).dump() + "\n";
    if (observation.source_frame_ids.size() > 1) ++reduced.multi_frame_observations;
  }
  for (const auto& value : result.numeric_values) {
    reduced.bytes += vision::numeric_value_to_json(value).dump() + "\n";
  }
  reduced.observations = result.text_observations.size();
  return reduced;
}

Batches chunk(const std::vector<OcrSampleDetections>& samples, std::size_t size) {
  Batches batches;
  for (std::size_t first = 0; first < samples.size(); first += size) {
    const auto begin = samples.begin() + static_cast<std::ptrdiff_t>(first);
    const auto end = samples.begin() +
        static_cast<std::ptrdiff_t>(std::min(samples.size(), first + size));
    batches.emplace_back(begin, end);
  }
  return batches;
}

void test_reduction_is_independent_of_batching() {
  const std::vector<OcrSampleDetections> samples = samples_with_a_miss(load_fixture());
  std::vector<std::int64_t> timestamps;
  for (const auto& sample : samples) timestamps.push_back(sample.timestamp_us);
  const vision::OcrSamplePlan plan = plan_for(timestamps);

  const ReducedText reference = reduce(plan, {samples});
  require(reference.observations > 0, "the fixture reconciles to observations");
  require(reference.multi_frame_observations > 0,
          "the fixture has text seen in several frames");

  // Fixed seeds keep the shuffled completion orders reproducible.
  constexpr std::array<std::uint32_t, 3> kShuffleSeeds = {1, 2, 3};
  for (const std::size_t size : {std::size_t{1}, std::size_t{3}, std::size_t{8}, samples.size()}) {
    const Batches in_order = chunk(samples, size);
    std::vector<Batches> completions = {in_order};
    Batches reversed = in_order;
    std::reverse(reversed.begin(), reversed.end());
    completions.push_back(reversed);
    for (const std::uint32_t seed : kShuffleSeeds) {
      std::mt19937 generator(seed);
      Batches shuffled = in_order;
      std::shuffle(shuffled.begin(), shuffled.end(), generator);
      for (auto& batch : shuffled) std::shuffle(batch.begin(), batch.end(), generator);
      completions.push_back(std::move(shuffled));
    }
    for (std::size_t order = 0; order < completions.size(); ++order) {
      require(reduce(plan, completions[order]).bytes == reference.bytes,
              "batch size " + std::to_string(size) + ", completion order " +
                  std::to_string(order) + " reduces to the single-batch output");
    }
  }
  std::cout << "  fixture: " << samples.size() << " samples, " << reference.observations
            << " observations (" << reference.multi_frame_observations
            << " seen in several frames)\n";
}

}  // namespace

int main() {
  test_codec_golden_bytes();
  test_codec_rejects();
  test_batch_policy();
  test_assembly_rejects_bad_coverage();
  test_reduction_is_independent_of_batching();
  std::cout << "All OCR frame batch tests passed.\n";
  return 0;
}
