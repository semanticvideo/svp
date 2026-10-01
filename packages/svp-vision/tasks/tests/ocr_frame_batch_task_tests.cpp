#include "svp/exec/canonical_json.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace exec = svp::exec;
namespace vision = svp::vision;
namespace tasks = svp::vision::tasks;
using Json = nlohmann::json;

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

// A well-formed decoder identity no real ffmpeg has.
const std::string kTestFfmpegBuild = "b3:" + std::string(64, 'a');

vision::PpOcrOptions explicit_pp_ocr() {
  vision::PpOcrOptions options;
  options.det_threads = {.intra_op = 4, .inter_op = 1};
  options.rec_threads = {.intra_op = 2, .inter_op = 1};
  options.recognition_parallel_workers = 2;
  return options;
}

vision::OcrSamplePlan small_plan() {
  vision::OcrTemporalSamplingResult sampling;
  sampling.timestamps_us = {0, 1'000'000, 2'000'000, 3'000'000, 4'000'000};
  return vision::make_ocr_sample_plan(sampling, 1920, 1080);
}

tasks::OcrFrameBatchParameters small_parameters() {
  tasks::OcrFrameBatchParameters parameters;
  parameters.samples = {{.ordinal = 3, .timestamp_us = 3'000'000},
                        {.ordinal = 4, .timestamp_us = 4'000'000}};
  parameters.frame_width = 1920;
  parameters.frame_height = 1080;
  parameters.pp_ocr = explicit_pp_ocr();
  parameters.ffmpeg_build = kTestFfmpegBuild;
  return parameters;
}

exec::TaskModelRef fake_ref(const std::string& model_id, std::uint8_t fill) {
  exec::Blake3Digest digest{};
  digest.fill(fill);
  return exec::TaskModelRef{
      .model_id = model_id,
      .model_bundle_id = model_id + "@test+blake3_" + exec::blake3_hex(digest).substr(0, 12),
      .bundle_blake3 = digest};
}

tasks::OcrFrameBatchTaskInputs task_inputs() {
  const vision::PpOcrOptions pp_ocr = explicit_pp_ocr();
  exec::Blake3Digest source{};
  source.fill(0x11);
  return tasks::OcrFrameBatchTaskInputs{
      .build_session_id = "bs_test",
      .depends_on = {"task.plan.ingest"},
      .source = exec::ArtifactRef{.blake3 = source,
                                  .bytes = 1234,
                                  .media_type = "video/mp4",
                                  .role = std::string(tasks::kOcrFrameBatchSourceRole)},
      .model_refs = {fake_ref(pp_ocr.recognizer_model_id, 0x22),
                     fake_ref(pp_ocr.detector_model_id, 0x33)},
      .pp_ocr = pp_ocr,
      .ffmpeg_build = kTestFfmpegBuild,
      .batch_policy = {},
  };
}

void test_parameters_golden_bytes() {
  const Json value = tasks::ocr_frame_batch_parameters_to_json(small_parameters());
  const std::string expected =
      R"({"decode":{"ffmpeg_build":"b3:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
      R"("frame_height":1080,"frame_width":1920},)"
      R"("detector":{"box_thresh":0.6,"execution_mode":"","graph_optimization_level":-1,)"
      R"("limit_side_len":960,"model_id":"model_pp_ocrv6_medium_det",)"
      R"("threads":{"inter_op":1,"intra_op":4},"thresh":0.3,"unclip_ratio":1.5},)"
      R"("execution_provider":"cpu",)"
      R"("recognizer":{"execution_mode":"","graph_optimization_level":-1,"image_height":48,)"
      R"("max_width":3200,"min_text_score":0.0,"model_id":"model_pp_ocrv6_medium_rec",)"
      R"("parallel_min_boxes":16,"parallel_workers":2,)"
      R"("threads":{"inter_op":1,"intra_op":2}},)"
      R"("samples":{"ordinals":[3,4],"timestamps_us":[3000000,4000000]}})";
  require(exec::encode_canonical_json(value) == expected, "parameters golden bytes");

  const tasks::OcrFrameBatchParameters decoded =
      tasks::ocr_frame_batch_parameters_from_json(value);
  require(tasks::ocr_frame_batch_parameters_to_json(decoded) == value,
          "parameters round trip");
  require(decoded.samples == small_parameters().samples, "samples round trip");
  require(decoded.pp_ocr.det_threads.intra_op == 4 &&
              decoded.pp_ocr.rec_threads.intra_op == 2 &&
              decoded.pp_ocr.recognition_parallel_workers == 2,
          "thread counts round trip");
}

void test_parameter_validator_rejects() {
  const Json valid = tasks::ocr_frame_batch_parameters_to_json(small_parameters());
  require(!tasks::validate_ocr_frame_batch_parameters(valid).has_value(), "valid accepted");

  const std::vector<std::pair<std::string, std::function<void(Json&)>>> breakages = {
      {"unknown top-level field", [](Json& v) { v["extra"] = 1; }},
      {"missing decode", [](Json& v) { v.erase("decode"); }},
      {"unknown detector field", [](Json& v) { v["detector"]["seed"] = 1; }},
      {"runtime-chosen thread count",
       [](Json& v) { v["detector"]["threads"]["intra_op"] = 0; }},
      {"zero recognition workers", [](Json& v) { v["recognizer"]["parallel_workers"] = 0; }},
      {"empty samples",
       [](Json& v) {
         v["samples"]["ordinals"] = Json::array();
         v["samples"]["timestamps_us"] = Json::array();
       }},
      {"length mismatch", [](Json& v) { v["samples"]["timestamps_us"].push_back(5'000'000); }},
      {"descending ordinals", [](Json& v) { v["samples"]["ordinals"] = Json::array({4, 3}); }},
      {"duplicate timestamps",
       [](Json& v) { v["samples"]["timestamps_us"] = Json::array({3'000'000, 3'000'000}); }},
      {"negative timestamp",
       [](Json& v) { v["samples"]["timestamps_us"] = Json::array({-1, 4'000'000}); }},
      {"zero frame width", [](Json& v) { v["decode"]["frame_width"] = 0; }},
      {"missing decoder identity", [](Json& v) { v["decode"].erase("ffmpeg_build"); }},
      {"malformed decoder identity", [](Json& v) { v["decode"]["ffmpeg_build"] = "9.0.2"; }},
      {"frame beyond the OCR decode bound",
       [](Json& v) { v["decode"]["frame_width"] = vision::kOcrMaxFrameDimension + 1; }},
      {"threshold above 1", [](Json& v) { v["detector"]["thresh"] = 1.5; }},
      {"integer for a float field", [](Json& v) { v["detector"]["unclip_ratio"] = 2; }},
      {"unnamed graph level", [](Json& v) { v["detector"]["graph_optimization_level"] = 5; }},
      {"unknown execution mode", [](Json& v) { v["recognizer"]["execution_mode"] = "fast"; }},
      {"unknown provider", [](Json& v) { v["execution_provider"] = "gpu"; }},
      {"non-canonical model id", [](Json& v) { v["detector"]["model_id"] = "PP-OCR"; }},
      {"string ordinal", [](Json& v) { v["samples"]["ordinals"][0] = "3"; }},
  };
  for (const auto& [name, breakage] : breakages) {
    Json broken = valid;
    breakage(broken);
    require(tasks::validate_ocr_frame_batch_parameters(broken).has_value(),
            "validator must reject: " + name);
  }
}

void test_parameters_require_explicit_threads() {
  tasks::OcrFrameBatchParameters parameters = small_parameters();
  parameters.pp_ocr.rec_threads.intra_op = svp::models::kRuntimeChoosesThreadCount;
  require_throws<std::invalid_argument>(
      [&] { (void)tasks::ocr_frame_batch_parameters_to_json(parameters); },
      "a runtime-chosen thread count cannot be sent to a worker");

  parameters = small_parameters();
  parameters.pp_ocr.det_graph_optimization_level = 7;
  const Json value = tasks::ocr_frame_batch_parameters_to_json(parameters);
  require(value["detector"]["graph_optimization_level"] == -1,
          "an unnamed graph level is sent as the runtime default it behaves as");
}

void test_task_spec() {
  const vision::OcrSamplePlan plan = small_plan();
  const vision::OcrSampleBatch batch{.first_ordinal = 3, .count = 2};
  const exec::TaskSpec spec = tasks::make_ocr_frame_batch_task_spec(task_inputs(), plan, batch);
  require(spec.task_id == "task.ocr.frame_batch.samples_000003_000004", "task id");
  require(spec.task_type == tasks::kOcrFrameBatchTaskType && spec.task_type_version == 1,
          "task type");
  require(spec.model_refs.size() == 2 &&
              spec.model_refs[0].model_id < spec.model_refs[1].model_id,
          "model refs sorted by model_id");
  require(spec.inputs.size() == 1 && spec.inputs.contains("source"), "source input");
  require(spec.parameters == tasks::ocr_frame_batch_parameters_to_json(small_parameters()),
          "spec parameters are the batch's plan slice");
  require(spec.resources.est_peak_rss_mb == tasks::kOcrFrameBatchEstimatedPeakRssMb &&
              spec.resources.est_cpu_threads == 4 && spec.resources.est_seconds >= 1,
          "resources");
  exec::validate_task_spec(spec);
  require(exec::decode_task_spec(exec::encode_task_spec(spec)) == spec, "spec round trip");

  const exec::TaskOrderKey key = tasks::ocr_frame_batch_order_key(batch);
  require(key.lane == tasks::kOcrFrameBatchLane && key.ordinals == std::vector<std::uint64_t>{3},
          "order key");

  // The cache key changes with any output-affecting parameter.
  tasks::OcrFrameBatchTaskInputs other_threads = task_inputs();
  other_threads.pp_ocr.rec_threads.intra_op = 3;
  require(tasks::make_ocr_frame_batch_task_spec(other_threads, plan, batch).cache_key !=
              spec.cache_key,
          "thread counts are part of the cache key");

  require_throws<std::invalid_argument>(
      [&] {
        (void)tasks::make_ocr_frame_batch_task_spec(task_inputs(), plan,
                                                    {.first_ordinal = 4, .count = 2});
      },
      "a batch outside the plan is rejected");

  exec::TaskTypeRegistry registry;
  tasks::register_ocr_frame_batch_task(registry, {});
  (void)registry.admit(spec);
  exec::TaskSpec bad = spec;
  bad.parameters["detector"]["threads"]["intra_op"] = 0;
  bad.parameters_blake3 = exec::compute_parameters_blake3(bad.parameters);
  require_throws<exec::ExecError>([&] { (void)registry.admit(bad); },
                                  "the registry rejects invalid parameters");
}

vision::OcrSampleDetections ok_record(std::uint64_t ordinal, std::int64_t timestamp_us) {
  vision::OcrSampleDetections record;
  record.sample_ordinal = ordinal;
  record.timestamp_us = timestamp_us;
  record.frame_width = 1920;
  record.frame_height = 1080;
  record.detections = {{.text = "SALE", .confidence = 0.75, .bbox_left = 1,
                        .bbox_top = 2, .bbox_right = 30, .bbox_bottom = 12}};
  return record;
}

void test_read_output() {
  const exec::TaskSpec spec = tasks::make_ocr_frame_batch_task_spec(
      task_inputs(), small_plan(), {.first_ordinal = 3, .count = 2});
  const std::vector<vision::OcrSampleDetections> records = {ok_record(3, 3'000'000),
                                                            ok_record(4, 4'000'000)};
  const std::string payload = vision::encode_ocr_sample_detections_jsonl(records);
  require(tasks::read_ocr_frame_batch_output(spec, payload) == records, "output read back");

  // Each payload line is svp-exec canonical JSON.
  std::size_t start = 0;
  while (start < payload.size()) {
    const std::size_t end = payload.find('\n', start);
    const std::string line = payload.substr(start, end - start);
    require(exec::encode_canonical_json(exec::decode_canonical_json(line)) == line,
            "payload line is canonical JSON");
    start = end + 1;
  }

  require_throws<std::invalid_argument>(
      [&] {
        (void)tasks::read_ocr_frame_batch_output(
            spec, vision::encode_ocr_sample_detections_jsonl(
                      std::vector<vision::OcrSampleDetections>{records[0]}));
      },
      "a missing record is rejected");
  require_throws<std::invalid_argument>(
      [&] {
        (void)tasks::read_ocr_frame_batch_output(
            spec, vision::encode_ocr_sample_detections_jsonl(
                      std::vector<vision::OcrSampleDetections>{records[1], records[0]}));
      },
      "records out of spec order are rejected");
}

void test_execute_failures() {
  const std::filesystem::path empty_cache =
      std::filesystem::temp_directory_path() / "svp-ocr-task-tests-empty-cache";
  std::filesystem::create_directories(empty_cache);
  // /bin/echo answers `-version` like a program that is not the coordinator's
  // ffmpeg: it runs, and its "build" is something else.
  const std::filesystem::path stand_in_ffmpeg = "/bin/echo";
  const std::optional<std::string> stand_in_build =
      tasks::ffmpeg_build_identity(stand_in_ffmpeg);
  require(stand_in_build.has_value(), "the stand-in decoder has an identity");
  require(!tasks::ffmpeg_build_identity("/nonexistent/ffmpeg").has_value(),
          "a missing program has no identity");

  exec::ResolvedInputs inputs;
  const exec::CancellationToken token;
  const auto run = [&](const std::filesystem::path& ffmpeg, const std::string& wanted_build,
                       bool drop_model_ref) {
    exec::TaskTypeRegistry registry;
    tasks::register_ocr_frame_batch_task(
        registry, {.model_cache_root = empty_cache, .ffmpeg_path = ffmpeg, .write_output = {}});
    tasks::OcrFrameBatchTaskInputs task = task_inputs();
    task.ffmpeg_build = wanted_build;
    exec::TaskSpec spec = tasks::make_ocr_frame_batch_task_spec(
        task, small_plan(), {.first_ordinal = 0, .count = 1});
    if (drop_model_ref) {
      spec.model_refs.pop_back();
    }
    inputs.clear();
    inputs.emplace("source", exec::ResolvedInput{.ref = spec.inputs.at("source"),
                                                 .path = "/nonexistent.mp4"});
    return registry.execute(spec, inputs, token);
  };
  const auto failed_with = [](const exec::TaskResult& result, const std::string& code,
                              bool retryable) {
    return result.status == exec::TaskStatus::failed && result.error &&
           result.error->code == code && result.error->retryable == retryable;
  };

  require(failed_with(run("/nonexistent/ffmpeg", kTestFfmpegBuild, false),
                      "decode_unavailable", true),
          "a runtime without ffmpeg fails retryably");
  require(failed_with(run(stand_in_ffmpeg, kTestFfmpegBuild, false), "decoder_mismatch", true),
          "a runtime whose ffmpeg is another build fails retryably");
  require(failed_with(run(stand_in_ffmpeg, *stand_in_build, false), "ocr_unavailable", true),
          "a worker without the models fails retryably");
  require(failed_with(run(stand_in_ffmpeg, *stand_in_build, true), "invalid_model_refs", false),
          "model refs that do not name both models are a permanent failure");

  exec::TaskTypeRegistry resolving;
  tasks::register_ocr_frame_batch_task(
      resolving, {.model_cache_root = {},
                  .ffmpeg_path = stand_in_ffmpeg,
                  .write_output = {},
                  .model_cache_for = [](const exec::TaskSpec&) -> std::filesystem::path {
                    throw std::runtime_error("bundle not installed");
                  }});
  tasks::OcrFrameBatchTaskInputs task = task_inputs();
  task.ffmpeg_build = *stand_in_build;
  const exec::TaskSpec spec = tasks::make_ocr_frame_batch_task_spec(
      task, small_plan(), {.first_ordinal = 0, .count = 1});
  require(failed_with(resolving.execute(spec, inputs, token), "model_unavailable", true),
          "a runtime that cannot provide the named bundles fails retryably");
  std::filesystem::remove_all(empty_cache);
}

}  // namespace

int main() {
  test_parameters_golden_bytes();
  test_parameter_validator_rejects();
  test_parameters_require_explicit_threads();
  test_task_spec();
  test_read_output();
  test_execute_failures();
  std::cout << "All ocr.frame_batch task tests passed.\n";
  return 0;
}
