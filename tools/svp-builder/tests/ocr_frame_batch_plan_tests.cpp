// The OCR stage as frame-batch tasks, without running OCR: the graph
// splices batches between the OCR stage's dependencies and its reducer, a
// resumed build recovers the recorded partition, committed batch outputs
// reach the reducer, the in-process artifact access serves the source and
// batch outputs, and calibration records round-trip.

#include "calibration/ocr_capacity_calibration.hpp"
#include "engine/committed_stage_results.hpp"
#include "engine/ocr_frame_batch_plan.hpp"
#include "engine/stage_output_access.hpp"
#include "engine/stage_task_plan.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace svp::builder::engine;

int g_failures = 0;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

std::vector<PlannedStageTask> package_tasks() {
  StageTaskPlanInputs inputs;
  inputs.stage_plan =
      svp::builder::execution_plan_for_stage(svp::builder::BuildStage::package_skeleton);
  inputs.single_video_heavy_lanes = 2;
  return plan_stage_tasks(inputs);
}

svp::exec::TaskModelRef fake_ref(const std::string& model_id, std::uint8_t fill) {
  svp::exec::Blake3Digest digest{};
  digest.fill(fill);
  return svp::exec::TaskModelRef{
      .model_id = model_id,
      .model_bundle_id = model_id + "@test+blake3_" + svp::exec::blake3_hex(digest).substr(0, 12),
      .bundle_blake3 = digest};
}

OcrWorkPlan work_plan(std::size_t samples) {
  OcrWorkPlan work;
  svp::vision::OcrTemporalSamplingResult sampling;
  for (std::size_t index = 0; index < samples; ++index) {
    sampling.timestamps_us.push_back(static_cast<std::int64_t>(index) * 1'000'000);
  }
  work.samples = svp::vision::make_ocr_sample_plan(sampling, 1920, 1080);
  work.pp_ocr.det_threads = {.intra_op = 4, .inter_op = 1};
  work.pp_ocr.rec_threads = {.intra_op = 2, .inter_op = 1};
  work.pp_ocr.recognition_parallel_workers = 2;
  work.model_refs = {fake_ref(work.pp_ocr.detector_model_id, 0x33),
                     fake_ref(work.pp_ocr.recognizer_model_id, 0x22)};
  work.ffmpeg_build = "b3:" + std::string(64, 'c');
  svp::exec::Blake3Digest source{};
  source.fill(0x11);
  work.source = svp::exec::ArtifactRef{
      .blake3 = source,
      .bytes = 4096,
      .media_type = "application/octet-stream",
      .role = std::string(svp::vision::tasks::kOcrFrameBatchSourceRole)};
  work.source_path = "/nonexistent/source.mp4";
  return work;
}

void test_batches_are_spliced_before_the_reducer() {
  const auto tasks = package_tasks();
  const std::vector<std::string> deps = ocr_stage_dependencies(tasks);
  const svp::vision::OcrBatchPolicy policy{.target_task_seconds = 3.0,
                                           .estimated_seconds_per_sample = 1.0};
  const OcrFrameBatchPlan batches =
      make_ocr_frame_batch_plan(work_plan(10), policy, "bs_test", deps);
  require(batches.batches.size() == 4, "10 samples at 3 per batch make 4 batches");
  const svp::exec::TaskGraph graph =
      make_build_task_graph(tasks, "bs_test", "b3:inputs", &batches);
  require(graph.size() == tasks.size() + batches.nodes.size(), "every batch is a graph node");

  const std::string reducer(stage_task_id(StageTaskKind::ocr));
  const auto& reducer_deps = graph.node(graph.find(reducer).value()).spec.depends_on;
  for (const svp::exec::TaskNode& batch : batches.nodes) {
    require(std::find(reducer_deps.begin(), reducer_deps.end(), batch.spec.task_id) !=
                reducer_deps.end(),
            "the OCR reducer waits for " + batch.spec.task_id);
    require(batch.spec.depends_on == deps,
            "a batch depends on exactly what the OCR stage depended on");
    require(batch.spec.task_type == svp::vision::tasks::kOcrFrameBatchTaskType,
            "batches are ocr.frame_batch tasks");
  }
  for (const std::string& dependency : deps) {
    require(std::find(reducer_deps.begin(), reducer_deps.end(), dependency) !=
                reducer_deps.end(),
            "the reducer keeps the stage's own dependency " + dependency);
  }
  require(make_build_task_graph(tasks, "bs_test", "b3:inputs", nullptr).size() == tasks.size(),
          "without batches the graph is the whole-stage graph");
}

// Nothing depends on how densely OCR samples: a plan four times denser than
// one sample per second of a ten-minute video, at a measured cost that puts
// one sample in each batch, still splices into one valid graph, and its
// partition still round-trips through the journal's task IDs.
void test_dense_sampling_scales() {
  const auto tasks = package_tasks();
  const std::vector<std::string> deps = ocr_stage_dependencies(tasks);
  constexpr std::size_t kDenseSamples = 4 * 600;
  for (const double measured_seconds_per_sample : {7.5, 0.4}) {
    const svp::vision::OcrBatchPolicy policy{
        .target_task_seconds = svp::vision::OcrBatchPolicy{}.target_task_seconds,
        .estimated_seconds_per_sample = measured_seconds_per_sample};
    const OcrFrameBatchPlan batches =
        make_ocr_frame_batch_plan(work_plan(kDenseSamples), policy, "bs_test", deps);
    const std::uint64_t per_batch = svp::vision::ocr_batch_sample_count(policy);
    require(batches.batches.size() == (kDenseSamples + per_batch - 1) / per_batch,
            "batch count follows the measured cost, not the sample count");
    const svp::exec::TaskGraph graph =
        make_build_task_graph(tasks, "bs_test", "b3:inputs", &batches);
    require(graph.size() == tasks.size() + batches.batches.size(), "dense plan graph builds");
    std::vector<std::string> ids;
    for (const svp::exec::TaskNode& node : batches.nodes) {
      ids.push_back(node.spec.task_id);
    }
    const auto partition = ocr_batches_from_task_ids(ids, kDenseSamples);
    require(partition && *partition == batches.batches, "dense partition round-trips");
  }
}

void test_resume_recovers_the_recorded_partition() {
  const auto tasks = package_tasks();
  const std::vector<std::string> deps = ocr_stage_dependencies(tasks);
  const OcrFrameBatchPlan recorded = make_ocr_frame_batch_plan(
      work_plan(10), {.target_task_seconds = 4.0, .estimated_seconds_per_sample = 1.0},
      "bs_test", deps);
  std::vector<std::string> journal_ids{std::string(stage_task_id(StageTaskKind::ocr)),
                                       "task.color.vstream_000"};
  for (const svp::exec::TaskNode& node : recorded.nodes) {
    journal_ids.push_back(node.spec.task_id);
  }
  const auto partition = ocr_batches_from_task_ids(journal_ids, 10);
  require(partition.has_value() && *partition == recorded.batches,
          "the recorded partition is read back from task IDs");

  // Another per-sample estimate now would cut differently; the recorded
  // partition still produces the very same specs.
  const OcrFrameBatchPlan resumed = make_ocr_frame_batch_plan_from_batches(
      work_plan(10), *partition, {.target_task_seconds = 4.0, .estimated_seconds_per_sample = 2.5},
      "bs_test", deps);
  bool same = resumed.nodes.size() == recorded.nodes.size();
  for (std::size_t index = 0; same && index < resumed.nodes.size(); ++index) {
    same = resumed.nodes[index].spec.task_id == recorded.nodes[index].spec.task_id &&
           resumed.nodes[index].spec.cache_key == recorded.nodes[index].spec.cache_key;
  }
  require(same, "a resumed build plans the recorded batches with the same cache keys");

  require(!ocr_batches_from_task_ids({"task.color.vstream_000"}, 10),
          "no recorded batches, no partition");
  require(!ocr_batches_from_task_ids({"task.ocr.frame_batch.samples_000000_000003",
                                      "task.ocr.frame_batch.samples_000005_000009"},
                                     10),
          "a gap is not a partition");
  require(!ocr_batches_from_task_ids({"task.ocr.frame_batch.samples_000000_000005",
                                      "task.ocr.frame_batch.samples_000004_000009"},
                                     10),
          "overlapping batches are not a partition");
  require(!ocr_batches_from_task_ids({"task.ocr.frame_batch.samples_000000_000009"}, 12),
          "a partition of another sample count is not reused");
}

void test_committed_batch_outputs_reach_the_reducer() {
  const OcrFrameBatchPlan batches = make_ocr_frame_batch_plan(
      work_plan(2), {.target_task_seconds = 10.0, .estimated_seconds_per_sample = 1.0},
      "bs_test", {});
  const svp::exec::TaskSpec& spec = batches.nodes.front().spec;
  const std::string text = "{\"sample\":0}\n";
  const svp::exec::FramePayload payload(reinterpret_cast<const std::byte*>(text.data()),
                                        reinterpret_cast<const std::byte*>(text.data()) +
                                            text.size());
  svp::exec::CommittedResult committed;
  committed.result.task_id = spec.task_id;
  committed.result.outputs = {svp::exec::make_artifact_ref(
      payload, "application/x-ndjson",
      std::string(svp::vision::tasks::kOcrFrameDetectionsRole))};
  committed.payloads = {payload};
  CommittedStageResults results;
  record_committed_result(results, spec, committed);
  require(results.task_output(spec.task_id) == payload,
          "a frame batch's output is kept as bytes for the reducer");
  require(!results.contains(spec.task_id), "a frame batch is not a stage result");
}

void test_in_process_access_serves_source_and_outputs() {
  StageOutputAccess access;
  const OcrWorkPlan work = work_plan(1);
  const OcrFrameBatchPlan batches = make_ocr_frame_batch_plan(
      work, {.target_task_seconds = 10.0, .estimated_seconds_per_sample = 1.0}, "bs_test", {});
  const svp::exec::TaskSpec& spec = batches.nodes.front().spec;
  bool unresolved = false;
  try {
    (void)access.resolve_inputs(spec);
  } catch (const svp::exec::ExecError& error) {
    unresolved = error.code() == svp::exec::ExecErrorCode::unresolved_input;
  }
  require(unresolved, "an unregistered source does not resolve");
  access.register_input(work.source, work.source_path);
  const svp::exec::ResolvedInputs inputs = access.resolve_inputs(spec);
  require(inputs.at("source").path == work.source_path, "the registered source resolves");

  const std::string text = "detections\n";
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                         text.size());
  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.outputs = {access.put(bytes, "application/x-ndjson",
                               std::string(svp::vision::tasks::kOcrFrameDetectionsRole))};
  const std::vector<svp::exec::FramePayload> read = access.read_outputs(result);
  require(read.size() == 1 && read.front().size() == text.size(),
          "a put output is read back by its digest");
}

void test_calibration_bounds_and_records() {
  constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  using svp::builder::calibration::ocr_calibration_max_slots;
  require(ocr_calibration_max_slots(7 * kGiB, 2 * kGiB, 10) == 3,
          "5 GiB beyond the reserve admits 3 slots of 1.5 GB");
  require(ocr_calibration_max_slots(64 * kGiB, 8 * kGiB, 4) == 4,
          "never more slots than logical CPUs");
  require(ocr_calibration_max_slots(1 * kGiB, 2 * kGiB, 10) == 1,
          "a Mac short of memory still measures one slot");

  svp::builder::calibration::OcrCalibration calibration;
  calibration.slots = 2;
  calibration.seconds_per_frame = 6.5;
  calibration.stopped_because = "3 slots added less than 10% throughput over 2";
  calibration.sweep = {{.slots = 1, .frames = 4, .wall_ms = 18000, .frames_per_second = 0.22},
                       {.slots = 2, .frames = 8, .wall_ms = 28000, .frames_per_second = 0.28}};
  const auto round_trip = svp::builder::calibration::ocr_calibration_from_json(
      svp::builder::calibration::ocr_calibration_to_json(calibration));
  require(round_trip.slots == 2 && round_trip.seconds_per_frame == 6.5 &&
              round_trip.sweep.size() == 2 && round_trip.sweep[1].wall_ms == 28000,
          "a calibration round-trips through its record");
}

}  // namespace

int main() {
  test_batches_are_spliced_before_the_reducer();
  test_resume_recovers_the_recorded_partition();
  test_dense_sampling_scales();
  test_committed_batch_outputs_reach_the_reducer();
  test_in_process_access_serves_source_and_outputs();
  test_calibration_bounds_and_records();
  if (g_failures != 0) {
    std::cerr << g_failures << " OCR frame batch plan check(s) failed\n";
    return 1;
  }
  std::cout << "svp-builder OCR frame batch plan tests passed\n";
  return 0;
}
