// Unit tests of the dispatched vision task types' contracts (no models
// needed): parameter codecs and validators, TaskSpec assembly, output
// readers, the item batch policy, and the failure results a runtime returns
// when it cannot start a task.

#include "svp/exec/canonical_json.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/frame_limits.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/tasks/depth_frame_batch_spec.hpp"
#include "svp/vision/tasks/dispatched_vision_tasks.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_spec.hpp"
#include "svp/vision/tasks/embed_text_batch_spec.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_crop_batch_spec.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <numeric>
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

const std::string kTestFfmpegBuild = "b3:" + std::string(64, 'a');

exec::TaskModelRef fake_ref(const std::string& model_id, std::uint8_t fill) {
  exec::Blake3Digest digest{};
  digest.fill(fill);
  return exec::TaskModelRef{
      .model_id = model_id,
      .model_bundle_id = model_id + "@test+blake3_" + exec::blake3_hex(digest).substr(0, 12),
      .bundle_blake3 = digest};
}

exec::ArtifactRef fake_source() {
  exec::Blake3Digest digest{};
  digest.fill(0x11);
  return exec::ArtifactRef{.blake3 = digest,
                           .bytes = 4096,
                           .media_type = "application/octet-stream",
                           .role = std::string(tasks::kOcrFrameBatchSourceRole)};
}

vision::PpOcrOptions explicit_pp_ocr() {
  vision::PpOcrOptions options;
  options.det_threads = {.intra_op = 4, .inter_op = 1};
  options.rec_threads = {.intra_op = 2, .inter_op = 1};
  options.recognition_parallel_workers = 2;
  return options;
}

tasks::OnnxModelParameters text_model() {
  return {.model_id = "model_nomic_embed_text_v1_5",
          .execution_provider = "cpu",
          .threads = {.intra_op = 4, .inter_op = 1}};
}

tasks::ItemBatchPolicy measured_policy(double seconds_per_item) {
  tasks::ItemBatchPolicy policy;
  policy.estimated_seconds_per_item = seconds_per_item;
  return policy;
}

std::vector<vision::EvidenceCropJob> crop_jobs(std::size_t count) {
  std::vector<vision::EvidenceCropJob> jobs;
  for (std::size_t index = 0; index < count; ++index) {
    jobs.push_back({.ordinal = index * 2,  // ordinals skip inputs without a crop
                    .seek_us = static_cast<std::int64_t>(index) * 500'000,
                    .left = 10,
                    .top = 20,
                    .width = 300,
                    .height = 40,
                    .image_format = "jpeg",
                    .jpeg_quality = 95});
  }
  return jobs;
}

// --- item batch policy ---------------------------------------------------

// Batches scale with the item count: 4x the items (denser sampling) gives
// 4x the batches at the same per-batch size, and every item is covered once,
// in order.
void test_partition_scales_with_items() {
  const tasks::ItemBatchPolicy policy = measured_policy(0.25);  // 40 per 10 s task
  const auto covers = [](const std::vector<tasks::ItemBatch>& batches, std::size_t count) {
    std::uint64_t next = 0;
    for (const tasks::ItemBatch& batch : batches) {
      if (batch.first != next || batch.count == 0) return false;
      next += batch.count;
    }
    return next == count;
  };
  for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{39},
                                  std::size_t{40}, std::size_t{41}, std::size_t{1000}}) {
    const auto batches = tasks::partition_items(count, policy, {});
    require(covers(batches, count), "partition covers every item once for " +
                                        std::to_string(count));
  }
  const auto base = tasks::partition_items(400, policy, {});
  const auto denser = tasks::partition_items(1600, policy, {});
  require(base.size() == 10 && denser.size() == 40,
          "4x the items give 4x the batches at the same measured cost");
  require(base.front().count == 40 && denser.front().count == 40,
          "batch size comes from the measured cost, not the item count");

  // A slower measured cost gives smaller batches.
  const auto slower = tasks::partition_items(400, measured_policy(1.0), {});
  require(slower.size() == 40 && slower.front().count == 10, "batch size follows the cost");
  // Cost far below the target: still finite batches, and one item at least.
  require(tasks::partition_items(3, measured_policy(1e-9), {}).size() == 1,
          "tiny costs put everything in one batch");
  require(tasks::partition_items(3, measured_policy(1e9), {}).size() == 3,
          "huge costs give one item per batch");
}

void test_partition_respects_byte_budgets() {
  tasks::ItemBatchPolicy policy = measured_policy(0.001);  // 10,000 per batch by time
  policy.max_parameter_bytes = 1000;
  policy.max_result_bytes = 10'000;
  // Parameters bound it: 100 bytes each, 10 per batch.
  auto batches = tasks::partition_items(35, policy, [](std::size_t) {
    return tasks::ItemBytes{.parameter_bytes = 100, .result_bytes = 1};
  });
  require(batches.size() == 4 && batches[0].count == 10 && batches[3].count == 5,
          "parameter bytes cut batches");
  // Results bound it: 2,500 bytes each, 4 per batch.
  batches = tasks::partition_items(9, policy, [](std::size_t) {
    return tasks::ItemBytes{.parameter_bytes = 1, .result_bytes = 2'500};
  });
  require(batches.size() == 3 && batches[0].count == 4 && batches[2].count == 1,
          "result bytes cut batches");
  // An item over a budget gets a batch of its own.
  batches = tasks::partition_items(3, policy, [](std::size_t index) {
    return tasks::ItemBytes{.parameter_bytes = index == 1 ? 5'000u : 10u, .result_bytes = 1};
  });
  require(batches.size() == 3 && batches[1].first == 1 && batches[1].count == 1,
          "an oversized item is alone");

  // The default budgets sit inside the protocol's frame limits.
  const tasks::ItemBatchPolicy defaults = measured_policy(1.0);
  require(defaults.max_parameter_bytes < exec::kDefaultMaxFrameHeaderBytes &&
              defaults.max_result_bytes < exec::kDefaultMaxFramePayloadBytes,
          "default budgets are below the frame limits");

  require_throws<std::invalid_argument>(
      [] { (void)tasks::partition_items(1, tasks::ItemBatchPolicy{}, {}); },
      "an unmeasured cost is rejected");
  tasks::ItemBatchPolicy no_budget = measured_policy(1.0);
  no_budget.max_result_bytes = 0;
  require_throws<std::invalid_argument>([&] { (void)tasks::partition_items(1, no_budget, {}); },
                                        "a zero byte budget is rejected");
  require(tasks::item_batch_estimated_seconds(measured_policy(0.25), 41) == 11,
          "estimated seconds round up");
  require(tasks::item_batch_estimated_seconds(measured_policy(1e-9), 1) == 1,
          "estimated seconds are at least one");
}

// --- ocr.crop_batch -------------------------------------------------------

void test_crop_parameters() {
  tasks::OcrCropBatchParameters parameters;
  parameters.jobs = crop_jobs(2);
  parameters.ffmpeg_build = kTestFfmpegBuild;
  parameters.pp_ocr = explicit_pp_ocr();
  const Json value = tasks::ocr_crop_batch_parameters_to_json(parameters);
  const std::string expected =
      R"({"decode":{"ffmpeg_build":"b3:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},)"
      R"("detector":{"box_thresh":0.6,"execution_mode":"","graph_optimization_level":-1,)"
      R"("limit_side_len":960,"model_id":"model_pp_ocrv6_medium_det",)"
      R"("threads":{"inter_op":1,"intra_op":4},"thresh":0.3,"unclip_ratio":1.5},)"
      R"("execution_provider":"cpu",)"
      R"("jobs":[{"height":40,"image_format":"jpeg","jpeg_quality":95,"left":10,"ordinal":0,)"
      R"("seek_us":0,"top":20,"width":300},)"
      R"({"height":40,"image_format":"jpeg","jpeg_quality":95,"left":10,"ordinal":2,)"
      R"("seek_us":500000,"top":20,"width":300}],)"
      R"("recognizer":{"execution_mode":"","graph_optimization_level":-1,"image_height":48,)"
      R"("max_width":3200,"min_text_score":0.0,"model_id":"model_pp_ocrv6_medium_rec",)"
      R"("parallel_min_boxes":16,"parallel_workers":2,)"
      R"("threads":{"inter_op":1,"intra_op":2}}})";
  require(exec::encode_canonical_json(value) == expected, "crop parameters golden bytes");
  const tasks::OcrCropBatchParameters decoded = tasks::ocr_crop_batch_parameters_from_json(value);
  require(decoded.jobs == parameters.jobs && decoded.ffmpeg_build == kTestFfmpegBuild,
          "crop parameters round-trip");

  const auto rejects = [&](const std::function<void(Json&)>& edit, const std::string& what) {
    Json bad = value;
    edit(bad);
    require(tasks::validate_ocr_crop_batch_parameters(bad).has_value(), "rejects " + what);
  };
  rejects([](Json& v) { v["jobs"] = Json::array(); }, "no jobs");
  rejects([](Json& v) { v["jobs"][1]["ordinal"] = 0; }, "non-ascending ordinals");
  rejects([](Json& v) { v["jobs"][0]["width"] = 0; }, "an empty crop");
  rejects([](Json& v) { v["jobs"][0]["left"] = -1; }, "a negative origin");
  rejects([](Json& v) { v["jobs"][0]["image_format"] = "gif"; }, "an unknown format");
  rejects([](Json& v) { v["jobs"][0]["jpeg_quality"] = 101; }, "a quality over 100");
  rejects([](Json& v) { v["jobs"][0]["path"] = "/tmp/x"; }, "an unknown job field");
  rejects([](Json& v) { v["decode"]["ffmpeg_build"] = "ffmpeg"; }, "a malformed decoder");
  rejects([](Json& v) { v["recognizer"]["threads"]["intra_op"] = 0; },
          "a thread count left to the worker");
  rejects([](Json& v) { v["command"] = "rm"; }, "an unknown top-level field");
}

tasks::OcrCropBatchTaskInputs crop_inputs() {
  const vision::PpOcrOptions pp_ocr = explicit_pp_ocr();
  return tasks::OcrCropBatchTaskInputs{
      .build_session_id = "bs_test",
      .depends_on = {},
      .source = fake_source(),
      .model_refs = {fake_ref(pp_ocr.recognizer_model_id, 0x22),
                     fake_ref(pp_ocr.detector_model_id, 0x33)},
      .pp_ocr = pp_ocr,
      .ffmpeg_build = kTestFfmpegBuild,
      .batch_policy = measured_policy(0.5)};
}

void test_crop_spec_and_output() {
  const std::vector<vision::EvidenceCropJob> jobs = crop_jobs(5);
  const exec::TaskSpec spec =
      tasks::make_ocr_crop_batch_task_spec(crop_inputs(), jobs, {.first = 1, .count = 3});
  require(spec.task_id == "task.ocr.crop_batch.items_000001_000003", "crop task id");
  require(spec.task_type == tasks::kOcrCropBatchTaskType && spec.inputs.size() == 1 &&
              spec.inputs.contains("source") && spec.model_refs.size() == 2 &&
              spec.model_refs[0].model_id < spec.model_refs[1].model_id,
          "crop spec names the source and both PP-OCR bundles, sorted");
  require(spec.resources.est_seconds == 2, "crop est seconds from the measured cost");
  require_throws<std::invalid_argument>(
      [&] { (void)tasks::make_ocr_crop_batch_task_spec(crop_inputs(), jobs, {.first = 4, .count = 2}); },
      "a batch past the jobs is rejected");

  // The registry admits the spec (schema, version) without running it.
  exec::TaskTypeRegistry registry;
  tasks::register_dispatched_vision_tasks(registry, {});
  (void)registry.admit(spec);

  // Output: records and images, matched to the spec's jobs.
  std::vector<std::byte> images = {std::byte{1}, std::byte{2}, std::byte{3}};
  const std::string records =
      R"({"data_bytes":2,"data_offset":0,"extracted":true,"ordinal":2,"roi_decoded":true,"roi_score":0.875,"roi_text":"AB"})" "\n"
      R"({"extracted":false,"ordinal":4})" "\n"
      R"({"data_bytes":1,"data_offset":2,"extracted":true,"ordinal":6,"roi_decoded":false,"roi_score":0.0,"roi_text":""})" "\n";
  std::vector<std::byte> record_bytes(records.size());
  std::memcpy(record_bytes.data(), records.data(), records.size());
  const std::vector<exec::ArtifactRef> outputs = {
      exec::make_artifact_ref(record_bytes, "application/x-ndjson",
                              std::string(tasks::kOcrCropRecordsRole)),
      exec::make_artifact_ref(images, "application/octet-stream",
                              std::string(tasks::kOcrCropImagesRole))};
  const auto outcomes =
      tasks::read_ocr_crop_batch_output(spec, outputs, {record_bytes, images});
  require(outcomes.size() == 3 && outcomes[0].extracted && outcomes[0].image.size() == 2 &&
              outcomes[0].roi.text == "AB" && outcomes[0].roi.score == 0.875 &&
              !outcomes[1].extracted && outcomes[2].image.size() == 1 &&
              !outcomes[2].roi.decoded,
          "crop output reads back per job");

  const std::string short_records = records.substr(0, records.find('\n') + 1);
  std::vector<std::byte> short_bytes(short_records.size());
  std::memcpy(short_bytes.data(), short_records.data(), short_records.size());
  require_throws<std::invalid_argument>(
      [&] {
        (void)tasks::read_ocr_crop_batch_output(
            spec,
            {exec::make_artifact_ref(short_bytes, "application/x-ndjson",
                                     std::string(tasks::kOcrCropRecordsRole)),
             outputs[1]},
            {short_bytes, images});
      },
      "a missing record is rejected (unit count)");
  std::vector<std::byte> few_images = {std::byte{1}};
  require_throws<std::invalid_argument>(
      [&] {
        (void)tasks::read_ocr_crop_batch_output(
            spec,
            {outputs[0], exec::make_artifact_ref(few_images, "application/octet-stream",
                                                 std::string(tasks::kOcrCropImagesRole))},
            {record_bytes, few_images});
      },
      "a data range past the images is rejected");
}

// --- embed.text_batch ------------------------------------------------------

void test_text_parameters_and_spec() {
  const std::vector<vision::TextEmbeddingItem> items = {
      {.id = "text_obs_000001", .text = "Revenue 2024"},
      {.id = "text_obs_000002", .text = "quote \" and \\ slash"},
      {.id = "text_obs_000003", .text = "Total: $1,234"}};
  const tasks::EmbedTextBatchTaskInputs inputs{.build_session_id = "bs_test",
                                               .depends_on = {},
                                               .model_ref = fake_ref(text_model().model_id, 0x44),
                                               .model = text_model(),
                                               .embedding_dim = 4,
                                               .batch_policy = measured_policy(0.02)};
  const exec::TaskSpec spec =
      tasks::make_embed_text_batch_task_spec(inputs, items, {.first = 1, .count = 2});
  require(spec.inputs.empty() && spec.task_id == "task.embed.text_batch.items_000001_000002",
          "text spec takes no inputs");
  const tasks::EmbedTextBatchParameters parameters =
      tasks::embed_text_batch_parameters_from_json(spec.parameters);
  require(parameters.items.size() == 2 && parameters.items[0].ordinal == 1 &&
              parameters.items[1].item == items[2] && parameters.embedding_dim == 4 &&
              parameters.model == text_model(),
          "text parameters carry the items and model");
  Json bad = spec.parameters;
  bad["threads"]["inter_op"] = 0;
  require(tasks::validate_embed_text_batch_parameters(bad).has_value(),
          "text parameters need explicit threads");
  bad = spec.parameters;
  bad["model_id"] = "../escape";
  require(tasks::validate_embed_text_batch_parameters(bad).has_value(),
          "text parameters need a canonical model id");

  // Two items: one vector, one stage blocker.
  const std::vector<float> vector = {0.5f, -0.5f, 0.5f, -0.5f};
  std::vector<std::byte> data(vector.size() * sizeof(float));
  std::memcpy(data.data(), vector.data(), data.size());
  const std::string records =
      R"({"data_bytes":16,"data_offset":0,"ordinal":1})" "\n"
      R"({"error":"ONNX text embedding inference failed for text_obs_000003: boom","ordinal":2})" "\n";
  std::vector<std::byte> record_bytes(records.size());
  std::memcpy(record_bytes.data(), records.data(), records.size());
  const auto outcomes = tasks::read_embed_text_batch_output(
      spec,
      {exec::make_artifact_ref(record_bytes, "application/x-ndjson",
                               std::string(tasks::kEmbedTextRecordsRole)),
       exec::make_artifact_ref(data, "application/octet-stream",
                               std::string(tasks::kEmbedTextVectorsRole))},
      {record_bytes, data});
  require(outcomes.size() == 2 && outcomes[0].vector == vector && outcomes[0].error.empty() &&
              outcomes[1].vector.empty() && !outcomes[1].error.empty(),
          "text output reads back vectors and failures");
}

// --- embed.keyframe_batch and depth.frame_batch ----------------------------

void test_keyframe_and_depth_specs() {
  const std::vector<vision::KeyframeEmbeddingItem> keyframes = {
      {.shot_id = "shot_000001", .pts_us = 0, .width = 640, .height = 360},
      {.shot_id = "shot_000002", .pts_us = 4'000'000, .width = 640, .height = 360}};
  const tasks::EmbedKeyframeBatchTaskInputs keyframe_inputs{
      .build_session_id = "bs_test",
      .depends_on = {},
      .source = fake_source(),
      .model_ref = fake_ref("model_nomic_embed_vision_v1_5", 0x55),
      .model = {.model_id = "model_nomic_embed_vision_v1_5",
                .execution_provider = "cpu",
                .threads = {.intra_op = 2, .inter_op = 1}},
      .embedding_dim = 768,
      .ffmpeg_build = kTestFfmpegBuild,
      .batch_policy = measured_policy(0.2)};
  const exec::TaskSpec keyframe_spec = tasks::make_embed_keyframe_batch_task_spec(
      keyframe_inputs, keyframes, {.first = 0, .count = 2});
  require(keyframe_spec.inputs.contains("source") &&
              tasks::embed_keyframe_batch_parameters_from_json(keyframe_spec.parameters)
                      .keyframes[1]
                      .item == keyframes[1],
          "keyframe spec carries its keyframes and source");

  vision::ColorRasterFrame frame{"frame_000016", 1'000'000, 2, 1, false, {{1, 2, 3}, {4, 5, 6}}};
  const tasks::DepthFrameItem item = tasks::depth_frame_item(frame, 3);
  std::vector<std::byte> rgb = {std::byte{1}, std::byte{2}, std::byte{3},
                                std::byte{4}, std::byte{5}, std::byte{6}};
  require(item.pixels_blake3 == exec::blake3_digest(std::span<const std::byte>(rgb)) &&
              item.ordinal == 3 && item.frame_id == "frame_000016" && item.width == 2,
          "a depth item names the frame's RGB24 pixels by BLAKE3");
  const tasks::DepthFrameBatchTaskInputs depth_inputs{
      .build_session_id = "bs_test",
      .depends_on = {},
      .source = fake_source(),
      .model_ref = fake_ref("model_depth_anything_v2_small", 0x66),
      .model = {.model_id = "model_depth_anything_v2_small",
                .execution_provider = "cpu",
                .threads = {.intra_op = 2, .inter_op = 1}},
      .ffmpeg_build = kTestFfmpegBuild,
      .batch_policy = measured_policy(0.5)};
  const exec::TaskSpec depth_spec =
      tasks::make_depth_frame_batch_task_spec(depth_inputs, {item}, {.first = 0, .count = 1});
  require(tasks::depth_frame_batch_parameters_from_json(depth_spec.parameters).frames[0] == item,
          "depth parameters round-trip");
  require(tasks::depth_frame_field_bytes(item) == 4, "a depth field is 2 bytes per pixel");

  const std::vector<std::uint16_t> field = {7, 65535};
  std::vector<std::byte> data(field.size() * sizeof(std::uint16_t));
  std::memcpy(data.data(), field.data(), data.size());
  const std::string records = R"({"data_bytes":4,"data_offset":0,"ordinal":3,"status":"ok"})" "\n";
  std::vector<std::byte> record_bytes(records.size());
  std::memcpy(record_bytes.data(), records.data(), records.size());
  const auto outcomes = tasks::read_depth_frame_batch_output(
      depth_spec,
      {exec::make_artifact_ref(record_bytes, "application/x-ndjson",
                               std::string(tasks::kDepthFrameRecordsRole)),
       exec::make_artifact_ref(data, "application/octet-stream",
                               std::string(tasks::kDepthFrameFieldsRole))},
      {record_bytes, data});
  require(outcomes.size() == 1 && outcomes[0].status == vision::DepthFrameStatus::ok &&
              outcomes[0].depth == field,
          "depth output reads back the field");
}

// --- failures before any item runs ----------------------------------------

// A runtime that cannot load the model: a worker fails the attempt
// retryably (another Mac takes it); the coordinator reports every item
// failed, so its stage does them itself. A spec naming another model is a
// permanent failure.
void test_start_failures() {
  const std::filesystem::path empty_cache =
      std::filesystem::temp_directory_path() / "svp-dispatched-empty-cache";
  std::filesystem::create_directories(empty_cache);
  const std::vector<vision::TextEmbeddingItem> items = {{.id = "a", .text = "alpha"},
                                                        {.id = "b", .text = "beta"}};
  const tasks::EmbedTextBatchTaskInputs inputs{.build_session_id = "bs_test",
                                               .depends_on = {},
                                               .model_ref = fake_ref(text_model().model_id, 0x44),
                                               .model = text_model(),
                                               .embedding_dim = 4,
                                               .batch_policy = measured_policy(0.02)};
  const exec::TaskSpec spec =
      tasks::make_embed_text_batch_task_spec(inputs, items, {.first = 0, .count = 2});
  std::vector<std::vector<std::byte>> stored;
  const auto run = [&](bool record_start_failures, const exec::TaskSpec& run_spec) {
    exec::TaskTypeRegistry registry;
    tasks::register_dispatched_vision_tasks(
        registry, tasks::DispatchedTaskEnvironment{
                      .model_cache_root = empty_cache,
                      .model_cache_for = {},
                      .ffmpeg_path = "ffmpeg",
                      .scratch_dir = empty_cache,
                      .write_output =
                          [&](std::span<const std::byte> bytes, std::string media_type,
                              std::string role) {
                            stored.emplace_back(bytes.begin(), bytes.end());
                            return exec::make_artifact_ref(bytes, std::move(media_type),
                                                           std::move(role));
                          },
                      .record_start_failures = record_start_failures});
    const exec::CancellationToken token;
    return registry.execute(run_spec, {}, token);
  };

  const exec::TaskResult on_worker = run(false, spec);
  require(on_worker.status == exec::TaskStatus::failed && on_worker.error &&
              on_worker.error->retryable && on_worker.error->code == "model_unavailable",
          "a worker without the model fails retryably");

  stored.clear();
  const exec::TaskResult on_coordinator = run(true, spec);
  require(on_coordinator.status == exec::TaskStatus::succeeded && stored.size() == 2,
          "the coordinator records the start failure as data");
  const auto outcomes = tasks::read_embed_text_batch_output(spec, on_coordinator.outputs, stored);
  require(outcomes.size() == 2 && !outcomes[0].error.empty() && !outcomes[1].error.empty(),
          "every item is reported failed, for the stage to embed itself");

  exec::TaskSpec other_model = spec;
  other_model.model_refs = {fake_ref("model_nomic_embed_vision_v1_5", 0x55)};
  const exec::TaskResult mismatched = run(false, other_model);
  require(mismatched.status == exec::TaskStatus::failed && mismatched.error &&
              !mismatched.error->retryable && mismatched.error->code == "invalid_model_refs",
          "model refs that disagree with the parameters are permanent");
  std::filesystem::remove_all(empty_cache);
}

}  // namespace

int main() {
  test_partition_scales_with_items();
  test_partition_respects_byte_budgets();
  test_crop_parameters();
  test_crop_spec_and_output();
  test_text_parameters_and_spec();
  test_keyframe_and_depth_specs();
  test_start_failures();
  std::cout << "dispatched task spec tests passed\n";
  return 0;
}
