// A tracking window as a unit of work (visual_entity_window.hpp):
//   * the outcome codec returns exactly what was encoded, every float bit
//     (non-finite values included), mask, and embedding;
//   * the fold accepts windows only in window order and never a window that
//     did not start;
//   * with --real-video, a source video, and the models
//     (SVP_TRACK_WINDOW_TEST_SOURCE, SVP_MODEL_CACHE_ROOT,
//     SVP_TRACK_WINDOW_TEST_FFMPEG, SVP_TRACK_WINDOW_TEST_FFPROBE), at more than one
//     tracking quality: every window computed alone, on a fresh thread with
//     fresh runtimes and in reverse order, encodes to the same bytes as the
//     same window computed in sequence after the windows before it, and the
//     fold of the separately computed windows equals the in-order stage
//     result. Skips without them.

#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_window.hpp"
#include "svp/vision/visual_entity_window_codec.hpp"
#include "svp/vision/visual_entity_window_fold.hpp"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace svp::vision;

int failures = 0;

void check(bool condition, const std::string& message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

TrackedRegion sample_region(int index, int width, int height) {
  TrackedRegion region;
  region.region_id = "region_" + std::to_string(index);
  region.entity_id = "entity_1";
  region.track_id = "track_1";
  region.frame_id = "frame_00000" + std::to_string(index);
  region.timestamp_us = 333'333LL * index;
  region.box_norm[0] = 0.1;
  region.box_norm[2] = 1.0 / 3.0;
  region.box_px[2] = 17;
  region.centroid_norm[1] = -0.0;
  region.screen_area_ratio = 0.0123456789012345678;
  region.median_inverse_depth = std::numeric_limits<double>::quiet_NaN();
  region.far_percentile_90 = std::numeric_limits<double>::infinity();
  region.mask_width = width;
  region.mask_height = height;
  region.mask_pixels.assign(static_cast<std::size_t>(width) * height, 0);
  for (std::size_t pixel = 0; pixel < region.mask_pixels.size(); pixel += 3 + index) {
    region.mask_pixels[pixel] = 1;
  }
  region.embedding = {0.25F, -1.5F, std::numeric_limits<float>::denorm_min(), 3.0e38F};
  region.embedding_model_id = "model_nomic_embed_vision_v1_5";
  region.confidence = 0.75;
  region.candidate_source = "objectness_detector";
  region.detector_category_index = index;
  return region;
}

VisualEntityWindowOutcome sample_outcome() {
  VisualEntityWindowOutcome outcome;
  outcome.status = VisualEntityWindowStatus::tracked;
  outcome.frames_attempted = 3;
  outcome.frames_decoded = 3;
  outcome.decoded_timestamps_us = {0, 333'333, 666'666};
  outcome.failures = {{"objectness_detector", "bad frame"}, {"depth_inference", "x"}};
  outcome.cut_evidence = {{.timestamp_us = 333'333,
                           .difference = 0.31,
                           .immediate_following_difference = 0.01,
                           .minimum_lookahead_difference = 1.0,
                           .is_sustained_transition = false,
                           .is_cut = true}};
  outcome.cut_timestamps_us = {333'333};
  outcome.detector_diagnostics = {.queries_evaluated = 900, .detections_emitted = 4};
  outcome.runtime_status = {.detector_loaded = true,
                            .detector_model_refs = {"model_rfdetr_nano_coco"},
                            .detector_model_identity = {{"model_bundle_id", "b"}},
                            .depth_loaded = true};
  outcome.tracker_result.regions = {sample_region(1, 8, 6), sample_region(2, 8, 6)};
  outcome.tracker_result.regions.back().mask_pixels.clear();  // a region without a mask
  outcome.tracker_result.entities.push_back(
      {.entity_id = "entity_1", .track_ids = {"track_1"}, .evidence_sources = {{{"a", 1}}}});
  outcome.tracker_result.tracks.push_back({.track_id = "track_1", .confidence = 0.5});
  outcome.tracker_result.processor_id = "processor_visual_entity_tracker";
  outcome.tracker_result.parameters_json = {{"threshold", 0.1}, {"count", 3}};
  return outcome;
}

bool same_double(double left, double right) {
  return std::memcmp(&left, &right, sizeof(double)) == 0;
}

void test_codec_round_trip() {
  const VisualEntityWindowOutcome outcome = sample_outcome();
  const std::vector<std::uint8_t> bytes = encode_visual_entity_window_outcome(outcome);
  const VisualEntityWindowOutcome decoded = decode_visual_entity_window_outcome(bytes);
  check(encode_visual_entity_window_outcome(decoded) == bytes,
        "a decoded outcome encodes to the same bytes");
  check(decoded.tracker_result.regions.size() == 2, "regions survive");
  const auto& before = outcome.tracker_result.regions.front();
  const auto& after = decoded.tracker_result.regions.front();
  check(after.mask_pixels == before.mask_pixels, "mask pixels survive");
  check(decoded.tracker_result.regions.back().mask_pixels.empty(), "a missing mask stays missing");
  check(after.embedding.size() == before.embedding.size() &&
            std::memcmp(after.embedding.data(), before.embedding.data(),
                        before.embedding.size() * sizeof(float)) == 0,
        "embedding bits survive");
  check(same_double(after.box_norm[2], before.box_norm[2]) &&
            same_double(after.screen_area_ratio, before.screen_area_ratio) &&
            same_double(after.centroid_norm[1], before.centroid_norm[1]) &&
            std::isnan(after.median_inverse_depth) && std::isinf(after.far_percentile_90),
        "double bits survive, -0.0 and non-finite values included");
  check(decoded.failures.size() == 2 && decoded.failures[1].component == "depth_inference",
        "failures keep their order");
  check(decoded.runtime_status.detector_model_refs == outcome.runtime_status.detector_model_refs,
        "runtime status survives");

  VisualEntityWindowOutcome not_binary = outcome;
  not_binary.tracker_result.regions.front().mask_pixels.front() = 2;
  bool rejected = false;
  try {
    (void)encode_visual_entity_window_outcome(not_binary);
  } catch (const VisualEntityWindowCodecError&) {
    rejected = true;
  }
  check(rejected, "a mask that is not binary is refused rather than altered");

  std::vector<std::uint8_t> truncated = bytes;
  truncated.resize(truncated.size() / 2);
  rejected = false;
  try {
    (void)decode_visual_entity_window_outcome(truncated);
  } catch (const VisualEntityWindowCodecError&) {
    rejected = true;
  }
  check(rejected, "truncated bytes are refused");
}

VisualEntityPipelinePlan two_window_plan() {
  VisualEntityPipelinePlan plan;
  plan.sampling = {.sample_interval_us = 500'000,
                   .window_duration_us = 2'000'000,
                   .window_overlap_us = 500'000};
  plan.duration_us = 3'000'000;
  plan.windows = make_visual_entity_sampling_plan(plan.duration_us, plan.sampling);
  return plan;
}

void test_fold_order() {
  VisualEntityPipelineOptions options;
  options.assembly.retain_artifacts_in_memory = true;
  const VisualEntityPipelinePlan plan = two_window_plan();
  check(plan.windows.size() == 2, "the fixture plan has two windows");
  VisualEntityWindowFold fold(options, plan, {}, nullptr);
  VisualEntityWindowOutcome decode_failed;
  decode_failed.failures = {{"frame_decode", "no frames"}};
  bool rejected = false;
  try {
    fold.append(1, decode_failed);
  } catch (const std::logic_error&) {
    rejected = true;
  }
  check(rejected, "the fold refuses a window out of order");
  VisualEntityWindowOutcome not_started;
  not_started.status = VisualEntityWindowStatus::not_started;
  rejected = false;
  try {
    fold.append(0, not_started);
  } catch (const std::logic_error&) {
    rejected = true;
  }
  check(rejected, "the fold refuses a window that did not start");
  fold.append(0, decode_failed);
  fold.append(1, decode_failed);
  const VisualEntityPipelineResult result = fold.finish();
  check(result.windows_processed == 2 && result.windows_succeeded == 0,
        "decode failures count as processed, not succeeded");
  check(result.failures.size() == 2, "the same failure in two windows is two failures");
  check(result.blocker == "visual entity tracking completed no windows",
        "a fold without tracked windows reports the stage's blocker");
}

// Windows computed in sequence on one thread with shared runtimes (the stage)
// versus each alone on a fresh thread with fresh runtimes, in reverse order
// (another Mac). Frames carry the same planned IDs both ways.
void check_windows_independent(const svp::media::MediaIngestPlan& media,
                               const std::filesystem::path& ffmpeg,
                               const std::filesystem::path& models, VisualTrackingQuality quality) {
  VisualEntityPipelineOptions options;
  options.quality = quality;
  options.detector.threads = {.intra_op = 4, .inter_op = 1};
  options.depth_threads = {.intra_op = 4, .inter_op = 1};
  options.embedding_threads = {.intra_op = 4, .inter_op = 1};
  options.assembly.retain_artifacts_in_memory = true;
  const VisualEntityPipelinePlan plan = plan_visual_entity_pipeline(media, options);
  const std::string name(visual_tracking_quality_name(quality));
  check(plan.windows.size() >= 2, name + ": the source spans at least two windows");

  FrameCatalog catalog;
  for (const auto& window : plan.windows) {
    for (const std::int64_t timestamp : window.timestamps_us) {
      (void)catalog.register_frame(timestamp, kVisualEntityTrackingFramePurpose);
    }
  }
  catalog.lock_to_plan();
  const auto identity_for = [&](std::size_t index) {
    VisualEntityWindowFrameIdentity identity;
    for (const std::int64_t timestamp : plan.windows[index].timestamps_us) {
      const std::size_t frame = *catalog.get_frame_index(timestamp);
      identity.planned_frame_ids.push_back(catalog.planned_entries()[frame].frame_id);
      identity.planned_frame_indices.push_back(frame);
    }
    return identity;
  };

  std::vector<std::vector<std::uint8_t>> in_order;
  {
    VisualEntityWindowRuntimes runtimes = load_visual_entity_window_runtimes(models, options);
    check(visual_entity_window_runtimes_complete(runtimes), name + ": the models load");
    for (std::size_t index = 0; index < plan.windows.size(); ++index) {
      in_order.push_back(encode_visual_entity_window_outcome(run_visual_entity_window(
          make_visual_entity_window_request(media, ffmpeg, options, plan, index), runtimes,
          identity_for(index))));
    }
  }
  std::vector<std::vector<std::uint8_t>> alone(plan.windows.size());
  for (std::size_t step = 0; step < plan.windows.size(); ++step) {
    const std::size_t index = plan.windows.size() - 1 - step;
    std::thread worker([&] {
      VisualEntityWindowRuntimes runtimes = load_visual_entity_window_runtimes(models, options);
      alone[index] = encode_visual_entity_window_outcome(run_visual_entity_window(
          make_visual_entity_window_request(media, ffmpeg, options, plan, index), runtimes,
          identity_for(index)));
    });
    worker.join();
  }
  for (std::size_t index = 0; index < plan.windows.size(); ++index) {
    check(alone[index] == in_order[index],
          name + ": window " + std::to_string(index) +
              " computed alone matches the window computed in sequence");
  }

  const auto fold_of = [&](const std::vector<std::vector<std::uint8_t>>& encoded) {
    VisualEntityWindowOutcome first = decode_visual_entity_window_outcome(encoded.front());
    VisualEntityWindowFold fold(options, plan, first.runtime_status, nullptr);
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      fold.append(index, decode_visual_entity_window_outcome(encoded[index]));
    }
    return fold.finish();
  };
  FrameCatalog staged_catalog = catalog;
  const VisualEntityPipelineResult staged = run_visual_entity_pipeline(
      media, ffmpeg, models, {}, &staged_catalog, options);
  const VisualEntityPipelineResult folded = fold_of(alone);
  check(staged.assembled.masks.size() == folded.assembled.masks.size(),
        name + ": the fold of separate windows has the stage's masks");
  bool same_masks = staged.assembled.masks.size() == folded.assembled.masks.size();
  for (std::size_t index = 0; same_masks && index < staged.assembled.masks.size(); ++index) {
    const auto& left = staged.assembled.masks[index];
    const auto& right = folded.assembled.masks[index];
    same_masks = left.mask_id == right.mask_id && left.frame_id == right.frame_id &&
                 left.entity_id == right.entity_id && left.track_id == right.track_id &&
                 left.rle_data == right.rle_data;
  }
  check(same_masks, name + ": every mask of the fold equals the stage's");
  check(staged.assembled.tracker_result.entities.size() ==
                folded.assembled.tracker_result.entities.size() &&
            staged.assembled.tracker_result.tracks.size() ==
                folded.assembled.tracker_result.tracks.size() &&
            staged.assembled.tracker_result.regions.size() ==
                folded.assembled.tracker_result.regions.size() &&
            staged.assembled.tracker_result.parameters_json ==
                folded.assembled.tracker_result.parameters_json,
        name + ": the fold's entities, tracks, regions, and parameters equal the stage's");
  std::cout << name << ": " << plan.windows.size() << " windows, "
            << folded.assembled.masks.size() << " masks\n";
}

void test_windows_are_independent() {
  const char* source = std::getenv("SVP_TRACK_WINDOW_TEST_SOURCE");
  const char* models = std::getenv("SVP_MODEL_CACHE_ROOT");
  const char* ffmpeg = std::getenv("SVP_TRACK_WINDOW_TEST_FFMPEG");
  const char* ffprobe = std::getenv("SVP_TRACK_WINDOW_TEST_FFPROBE");
  if (source == nullptr || models == nullptr || ffmpeg == nullptr || ffprobe == nullptr) {
    std::cout << "Skipping: set SVP_TRACK_WINDOW_TEST_SOURCE, SVP_MODEL_CACHE_ROOT, "
                 "SVP_TRACK_WINDOW_TEST_FFMPEG, and SVP_TRACK_WINDOW_TEST_FFPROBE to check "
                 "window independence on a real video\n";
    return;
  }
  const svp::media::MediaIngestPlan media = svp::media::build_media_ingest_plan(
      source, svp::media::probe_media_with_ffprobe(source, ffprobe));
  for (const VisualTrackingQuality quality :
       {VisualTrackingQuality::medium, VisualTrackingQuality::high}) {
    check_windows_independent(media, ffmpeg, models, quality);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--real-video") {
    test_windows_are_independent();
  } else {
    test_codec_round_trip();
    test_fold_order();
  }
  if (failures != 0) return 1;
  std::cout << "All visual entity window tests passed.\n";
  return 0;
}
