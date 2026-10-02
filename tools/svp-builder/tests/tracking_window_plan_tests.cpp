// The tracking stage as window tasks, without running tracking: one task per
// planned window whatever the duration or cadence, spliced between the
// tracking stage's dependencies and its fold with the rest of the graph
// unchanged (alongside OCR batches too); committed window outputs reach the
// fold; the stage's progress spans the windows and the fold; the tracking
// calibration sweep warms every slot and is bounded by memory; and tracking
// calibration records live beside, not over, OCR records.

#include "calibration/track_window_capacity_calibration.hpp"
#include "engine/build_task_graph.hpp"
#include "engine/committed_stage_results.hpp"
#include "engine/stage_task_plan.hpp"
#include "engine/tracking_window_plan.hpp"
#include "engine/tracking_window_progress.hpp"
#if defined(__APPLE__)
#include "workers/calibration_store.hpp"
#endif

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/clock.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/track_window_calibration_clip.hpp"
#include "svp/vision/tasks/track_window_task.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"
#include "svp/vision/visual_entity_window_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
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

std::vector<PlannedStageTask> package_tasks(bool serial) {
  StageTaskPlanInputs inputs;
  inputs.stage_plan =
      svp::builder::execution_plan_for_stage(svp::builder::BuildStage::package_skeleton);
  inputs.serial_pipeline = serial;
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

struct Fixture {
  TrackingWorkPlan work;
  svp::vision::FrameCatalog catalog;
};

Fixture fixture(svp::vision::VisualTrackingQuality quality, std::int64_t duration_us) {
  Fixture fixture;
  TrackingWorkPlan& work = fixture.work;
  work.options.quality = quality;
  work.options.detector.threads = {.intra_op = 4, .inter_op = 1};
  work.options.depth_threads = {.intra_op = 4, .inter_op = 1};
  work.options.embedding_threads = {.intra_op = 4, .inter_op = 1};
  const auto policy = svp::vision::visual_tracking_quality_policy(quality);
  work.plan.sampling = svp::vision::visual_entity_sampling_options(policy);
  work.plan.depth_schedule.periodic_interval_us = policy.depth_interval_us;
  work.plan.duration_us = duration_us;
  work.plan.windows =
      svp::vision::make_visual_entity_sampling_plan(duration_us, work.plan.sampling);
  work.model_refs = {fake_ref(work.options.detector.model_id, 0x21),
                     fake_ref(svp::vision::tasks::track_window_depth_model_id(), 0x22),
                     fake_ref(work.options.embedding_model_id, 0x23)};
  work.ffmpeg_build = "b3:" + std::string(64, 'c');
  work.frame_width = 640;
  work.frame_height = 360;
  svp::exec::Blake3Digest source{};
  source.fill(0x11);
  work.source = {.blake3 = source,
                 .bytes = 4096,
                 .media_type = "application/octet-stream",
                 .role = std::string(svp::vision::tasks::kTrackWindowSourceRole)};
  work.source_path = "/nonexistent/source.mp4";
  for (const auto& window : work.plan.windows) {
    for (const std::int64_t timestamp : window.timestamps_us) {
      (void)fixture.catalog.register_frame(timestamp,
                                           svp::vision::kVisualEntityTrackingFramePurpose);
    }
  }
  fixture.catalog.lock_to_plan();
  return fixture;
}

std::set<std::string> deps_of(const svp::exec::TaskGraph& graph, const std::string& id) {
  const auto& deps = graph.node(graph.find(id).value()).spec.depends_on;
  return {deps.begin(), deps.end()};
}

// For every stage task, the graph with windows has the same dependencies on
// other stages as the whole-stage graph, plus (for the fold only) the windows.
void test_windows_are_spliced_before_the_fold() {
  for (const bool serial : {false, true}) {
    const auto tasks = package_tasks(serial);
    Fixture f = fixture(svp::vision::VisualTrackingQuality::medium, 61'000'000);
    const std::vector<std::string> deps = tracking_stage_dependencies(tasks);
    const TrackingWindowPlan windows =
        make_tracking_window_plan(f.work, {}, "bs_test", deps, f.catalog);
    require(windows.nodes.size() == f.work.plan.windows.size(), "one task per planned window");
    const svp::exec::TaskGraph whole = make_stage_task_graph(tasks, "bs_test", "b3:inputs");
    const svp::exec::TaskGraph graph = make_split_build_task_graph(
        tasks, "bs_test", "b3:inputs", SplitStageTasks{.tracking_windows = &windows});
    require(graph.size() == whole.size() + windows.nodes.size(), "every window is a node");
    const std::string fold(stage_task_id(StageTaskKind::tracking));
    std::set<std::string> window_ids;
    for (const svp::exec::TaskNode& node : windows.nodes) {
      window_ids.insert(node.spec.task_id);
      require(node.spec.depends_on == deps,
              "a window depends on exactly what the tracking stage depended on");
      require(node.spec.task_type == svp::vision::tasks::kTrackWindowTaskType,
              "windows are track.window tasks");
    }
    for (std::size_t index = 0; index < whole.size(); ++index) {
      const std::string& id = whole.node(index).spec.task_id;
      std::set<std::string> expected = deps_of(whole, id);
      if (id == fold) expected.insert(window_ids.begin(), window_ids.end());
      require(deps_of(graph, id) == expected,
              std::string(serial ? "serial" : "parallel") + " stage " + id +
                  " keeps its dependencies (the fold also waits for every window)");
    }
  }
}

// OCR batches and tracking windows split in the same graph without touching
// each other's stage.
void test_both_stages_split_together() {
  const auto tasks = package_tasks(false);
  Fixture f = fixture(svp::vision::VisualTrackingQuality::low, 30'000'000);
  const TrackingWindowPlan windows = make_tracking_window_plan(
      f.work, {}, "bs_test", tracking_stage_dependencies(tasks), f.catalog);
  OcrFrameBatchPlan batches;
  batches.nodes.push_back(svp::exec::TaskNode{
      .spec = windows.nodes.front().spec, .order_key = {.lane = "x", .ordinals = {0}}});
  batches.nodes.front().spec.task_id = "task.ocr.frame_batch.samples_000000_000000";
  batches.nodes.front().spec.depends_on = ocr_stage_dependencies(tasks);
  const svp::exec::TaskGraph graph = make_split_build_task_graph(
      tasks, "bs_test", "b3:inputs",
      SplitStageTasks{.ocr_batches = &batches, .tracking_windows = &windows});
  const auto ocr_deps = deps_of(graph, std::string(stage_task_id(StageTaskKind::ocr)));
  const auto tracking_deps = deps_of(graph, std::string(stage_task_id(StageTaskKind::tracking)));
  require(ocr_deps.contains(batches.nodes.front().spec.task_id) &&
              !ocr_deps.contains(windows.nodes.front().spec.task_id),
          "the OCR reducer waits for batches, not windows");
  require(tracking_deps.contains(windows.nodes.back().spec.task_id) &&
              !tracking_deps.contains(batches.nodes.front().spec.task_id),
          "the tracking fold waits for windows, not batches");
}

// Nothing depends on the duration or the cadence: a two-hour video at the
// densest quality is simply more windows, each carrying its own frames.
void test_window_count_scales_with_the_plan() {
  const auto tasks = package_tasks(false);
  std::size_t previous_frames = 0;
  for (const auto quality :
       {svp::vision::VisualTrackingQuality::low, svp::vision::VisualTrackingQuality::medium,
        svp::vision::VisualTrackingQuality::high}) {
    std::size_t previous_windows = 0;
    std::size_t frames = 0;
    for (const std::int64_t duration_us : {6'000'000LL, 600'000'000LL, 7'200'000'000LL}) {
      Fixture f = fixture(quality, duration_us);
      const TrackingWindowPlan windows = make_tracking_window_plan(
          f.work, {}, "bs_test", tracking_stage_dependencies(tasks), f.catalog);
      require(windows.nodes.size() == f.work.plan.windows.size(),
              "one task per window at every duration and quality");
      require(windows.nodes.size() > previous_windows, "a longer video has more windows");
      previous_windows = windows.nodes.size();
      frames = 0;
      for (const auto& node : windows.nodes) {
        frames += svp::vision::tasks::track_window_parameters_from_json(node.spec.parameters)
                      .window.timestamps_us.size();
      }
    }
    require(frames > previous_frames, "a denser quality sends more frames");
    previous_frames = frames;
  }
}

void test_outputs_reach_the_fold() {
  Fixture f = fixture(svp::vision::VisualTrackingQuality::medium, 30'000'000);
  const TrackingWindowPlan windows =
      make_tracking_window_plan(f.work, {}, "bs_test", {}, f.catalog);
  svp::vision::VisualEntityWindowOutcome outcome;
  outcome.frames_attempted = f.work.plan.windows.front().timestamps_us.size();
  outcome.decoded_timestamps_us = {f.work.plan.windows.front().timestamps_us.front()};
  outcome.failures = {{"frame_decode", "fewer than two visual entity frames were decoded"}};
  const std::vector<std::uint8_t> encoded =
      svp::vision::encode_visual_entity_window_outcome(outcome);
  std::vector<std::byte> payload(encoded.size());
  std::memcpy(payload.data(), encoded.data(), encoded.size());
  svp::exec::CommittedResult committed;
  committed.result.task_id = windows.nodes.front().spec.task_id;
  committed.payloads = {payload};
  CommittedStageResults results;
  record_committed_result(results, windows.nodes.front().spec, committed);
  const std::vector<std::byte> stored = results.task_output(committed.result.task_id);
  const auto read = svp::vision::tasks::read_track_window_output(
      windows.nodes.front().spec,
      std::span(reinterpret_cast<const std::uint8_t*>(stored.data()), stored.size()));
  require(read.decoded_timestamps_us == outcome.decoded_timestamps_us,
          "a committed window output reaches the fold unchanged");
  outcome.decoded_timestamps_us = {f.work.plan.windows.back().timestamps_us.back()};
  const std::vector<std::uint8_t> foreign =
      svp::vision::encode_visual_entity_window_outcome(outcome);
  bool rejected = false;
  try {
    (void)svp::vision::tasks::read_track_window_output(windows.nodes.front().spec, foreign);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "an outcome of another window is refused");
}

class RecordingSink final : public svp::builder::BuildProgressSink {
 public:
  void emit(const svp::builder::ProgressEvent& event) override { events.push_back(event); }
  std::vector<svp::builder::ProgressEvent> events;
};

std::size_t count(const RecordingSink& sink, svp::builder::ProgressEventKind kind) {
  return static_cast<std::size_t>(std::count_if(
      sink.events.begin(), sink.events.end(),
      [kind](const svp::builder::ProgressEvent& event) { return event.kind == kind; }));
}

void test_progress_spans_windows_and_fold() {
  Fixture f = fixture(svp::vision::VisualTrackingQuality::medium, 61'000'000);
  const TrackingWindowPlan windows =
      make_tracking_window_plan(f.work, {}, "bs_test", {}, f.catalog);
  const svp::exec::SteadyClock clock;
  RecordingSink sink;
  TrackingWindowProgress progress(windows, sink, clock, true,
                                  {windows.nodes.front().spec.task_id});
  for (std::size_t index = 1; index < windows.nodes.size(); ++index) {
    const std::string lease = "lease_" + std::to_string(index);
    progress.observe({.kind = svp::exec::AttemptEventKind::leased,
                      .task_id = windows.nodes[index].spec.task_id,
                      .lease_id = lease});
    progress.observe({.kind = svp::exec::AttemptEventKind::committed,
                      .task_id = windows.nodes[index].spec.task_id,
                      .lease_id = lease});
  }
  require(count(sink, svp::builder::ProgressEventKind::stage_started) == 1,
          "the stage starts once, at the first lease");
  require(count(sink, svp::builder::ProgressEventKind::stage_completed) == 0,
          "committing every window does not complete the stage");
  progress.fold_started();
  progress.fold_finished();
  require(count(sink, svp::builder::ProgressEventKind::stage_started) == 1 &&
              count(sink, svp::builder::ProgressEventKind::stage_completed) == 1,
          "the fold completes the stage it did not start again");
  require(count(sink, svp::builder::ProgressEventKind::stage_progress) ==
              windows.nodes.size() - 1,
          "one progress event per committed window (restored windows count as done)");

  RecordingSink resumed_sink;
  std::set<std::string> all;
  for (const auto& node : windows.nodes) all.insert(node.spec.task_id);
  TrackingWindowProgress resumed(windows, resumed_sink, clock, true, all);
  resumed.fold_started();
  resumed.fold_finished();
  require(count(resumed_sink, svp::builder::ProgressEventKind::stage_started) == 1 &&
              count(resumed_sink, svp::builder::ProgressEventKind::stage_completed) == 1,
          "a fully resumed stage still starts and completes once");
}

void test_tracking_calibration_sweep() {
  Fixture f = fixture(svp::vision::VisualTrackingQuality::medium, 30'000'000);
  const svp::vision::tasks::TrackWindowTaskInputs inputs{
      .build_session_id = "bs_calibration",
      .depends_on = {},
      .source = f.work.source,
      .model_refs = f.work.model_refs,
      .options = f.work.options,
      .frame_width = 640,
      .frame_height = 360,
      .ffmpeg_build = f.work.ffmpeg_build,
      .cost = {}};
  const auto workload = svp::builder::calibration::track_window_calibration_workload(inputs);
  require(workload.items_per_batch == svp::vision::tasks::kTrackWindowCalibrationFrames,
          "a timed calibration task tracks the whole calibration window");
  for (std::size_t max_slots = 1; max_slots <= 4; ++max_slots) {
    const auto graph = svp::builder::calibration::capacity_calibration_graph(workload, max_slots);
    std::map<std::size_t, std::size_t> timed;
    for (const auto& [id, step] : graph.timed_step) ++timed[step];
    for (std::size_t step = 1; step <= max_slots; ++step) {
      require(timed[step] == step * svp::builder::calibration::kCapacityBatchesPerSlot,
              "step " + std::to_string(step) + " times exactly that many windows");
    }
  }
  constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  const std::size_t bounded =
      svp::builder::calibration::track_window_calibration_max_slots(inputs, 6 * kGiB, 2 * kGiB, 10);
  require(bounded >= 1 && bounded <= 3, "memory bounds the slots a sweep may try");
  require(svp::builder::calibration::track_window_calibration_max_slots(
              inputs, 512 * kGiB, 2 * kGiB, 6) == 6,
          "logical CPUs bound the slots a sweep may try");
}

#if defined(__APPLE__)
void test_calibration_records_by_kind() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "svp-tracking-calibration-store-test";
  std::filesystem::remove_all(directory);
  using svp::builder::workers::CalibrationStore;
  using svp::builder::workers::CapacityRecord;
  const CalibrationStore store(directory);
  CapacityRecord record;
  record.capacity = {.slots = 4, .seconds_per_item = 0.4, .sweep = {}, .stopped_because = "x"};
  record.measured_at = "2026-10-01T00:00:00Z";
  // One record per tracking workload: switching quality keeps both.
  const std::string medium = "track.window.0123456789abcdef";
  const std::string high = "track.window.fedcba9876543210";
  store.write_capacity("worker", medium, record);
  record.capacity.slots = 5;
  store.write_capacity("worker", high, record);
  require(store.read_capacity("worker", medium)->capacity.slots == 4 &&
              store.read_capacity("worker", high)->capacity.slots == 5,
          "records of two tracking workloads live side by side");
  require(!store.read_capacity("worker", "track.window").has_value(),
          "a workload's record is not another workload's");
  store.write_capacity("workerz", medium, record);
  store.remove("worker");
  require(!store.read_capacity("worker", medium).has_value() &&
              !store.read_capacity("worker", high).has_value(),
          "removing a Mac's records removes every tracking workload's");
  require(store.read_capacity("workerz", medium).has_value(), "another Mac's records stay");
  store.remove("workerz");
  require(!std::filesystem::exists(directory), "removing the last record removes the directory");
}
#endif

// True when `root` is a model cache that holds models. CI points
// SVP_MODEL_CACHE_ROOT at a directory with no model assets.
bool has_models(const char* root) {
  std::error_code error;
  return root != nullptr && *root != '\0' && std::filesystem::is_directory(root, error) &&
         !std::filesystem::is_empty(root, error);
}


// A Mac that cannot load the window runtimes does not split the stage. With
// the models (SVP_MODEL_CACHE_ROOT) a build's options load; options whose
// detector cannot load (or an empty cache) do not. Skips without the models.
void test_runtime_probe() {
  require(!tracking_runtimes_load("/nonexistent/models", {}),
          "nothing loads from a cache that does not exist");
  const char* models = std::getenv("SVP_MODEL_CACHE_ROOT");
  if (!has_models(models)) {
    std::cout << "skipping runtime probe with models: SVP_MODEL_CACHE_ROOT holds no models\n";
    return;
  }
  svp::vision::VisualEntityPipelineOptions options;
  options.detector.threads = {.intra_op = 2, .inter_op = 1};
  options.depth_threads = {.intra_op = 2, .inter_op = 1};
  options.embedding_threads = {.intra_op = 2, .inter_op = 1};
  require(tracking_runtimes_load(models, options), "a build's options load from the models");
  options.detector.confidence_threshold = 0.0;  // the detector refuses to load
  require(!tracking_runtimes_load(models, options), "a detector that cannot load is caught");
}

// A journal resumes the way it was built: windows when it recorded windows,
// the whole stage otherwise, with or without --distributed now; a new build
// splits only when --distributed.
void test_resume_follows_the_journal() {
  using svp::builder::RecoveryJournalMode;
  const std::vector<std::string> distributed_journal{
      "task.tracking.vstream_000", svp::vision::tasks::track_window_task_id(0),
      svp::vision::tasks::track_window_task_id(1)};
  const std::vector<std::string> local_journal{"task.tracking.vstream_000",
                                               "task.ocr.frame_batch.samples_000000_000003"};
  require(split_tracking_stage(RecoveryJournalMode::resume, false, distributed_journal),
          "a local --resume of a distributed journal runs its windows");
  require(!split_tracking_stage(RecoveryJournalMode::resume, true, local_journal),
          "a --distributed --resume of a local journal runs the whole stage");
  require(split_tracking_stage(RecoveryJournalMode::resume, true, distributed_journal) &&
              !split_tracking_stage(RecoveryJournalMode::resume, false, local_journal),
          "resuming in the same mode keeps the journal's shape");
  for (const auto mode : {RecoveryJournalMode::require_new, RecoveryJournalMode::fresh}) {
    require(split_tracking_stage(mode, true, {}) && !split_tracking_stage(mode, false, {}),
            "a new build splits exactly when --distributed");
  }
}

// The calibration sweep's windows run on workers, which return only clean
// windows: with the models and ffmpeg (SVP_MODEL_CACHE_ROOT,
// SVP_TRACK_WINDOW_TEST_FFMPEG), the warm-up and the timed window both
// succeed as a worker runs them. Skips without them.
void test_calibration_windows_are_clean_on_a_worker() {
  const char* models = std::getenv("SVP_MODEL_CACHE_ROOT");
  const char* ffmpeg = std::getenv("SVP_TRACK_WINDOW_TEST_FFMPEG");
  if (!has_models(models) || ffmpeg == nullptr) {
    std::cout << "skipping calibration windows on a worker: set SVP_MODEL_CACHE_ROOT (with models) and "
                 "SVP_TRACK_WINDOW_TEST_FFMPEG\n";
    return;
  }
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "svp-track-calibration-worker-test";
  std::filesystem::remove_all(directory);
  svp::vision::VisualEntityPipelineOptions options;
  options.quality = svp::vision::VisualTrackingQuality::medium;
  options.detector.threads = {.intra_op = 2, .inter_op = 1};
  options.depth_threads = {.intra_op = 2, .inter_op = 1};
  options.embedding_threads = {.intra_op = 2, .inter_op = 1};
  const auto policy = svp::vision::visual_tracking_quality_policy(options.quality);
  const std::filesystem::path clip = svp::vision::tasks::write_track_window_calibration_clip(
      ffmpeg, directory, policy.sample_interval_us);
  svp::exec::Blake3Digest digest{};
  digest.fill(0x44);
  const svp::vision::tasks::TrackWindowTaskInputs inputs{
      .build_session_id = "bs_calibration",
      .depends_on = {},
      .source = {.blake3 = digest,
                 .bytes = std::filesystem::file_size(clip),
                 .media_type = "application/octet-stream",
                 .role = std::string(svp::vision::tasks::kTrackWindowSourceRole)},
      .model_refs = svp::vision::tasks::track_window_model_refs(models, options),
      .options = options,
      .frame_width = svp::vision::tasks::kTrackWindowCalibrationFrameWidth,
      .frame_height = svp::vision::tasks::kTrackWindowCalibrationFrameHeight,
      .ffmpeg_build = svp::vision::tasks::ffmpeg_build_identity(ffmpeg).value_or(""),
      .cost = {}};
  const auto workload = svp::builder::calibration::track_window_calibration_workload(inputs);
  svp::exec::TaskTypeRegistry registry;
  svp::vision::tasks::register_track_window_task(
      registry, svp::vision::tasks::TrackWindowWorkerEnvironment{
                    .model_cache_root = models,
                    .ffmpeg_path = ffmpeg,
                    .write_output =
                        [](std::span<const std::byte> bytes, std::string media_type,
                           std::string role) {
                          return svp::exec::make_artifact_ref(bytes, std::move(media_type),
                                                              std::move(role));
                        },
                    .model_cache_for = {},
                    .record_start_failures = false});
  svp::exec::ResolvedInputs resolved;
  resolved.emplace(std::string(svp::vision::tasks::kTrackWindowSourceInput),
                   svp::exec::ResolvedInput{.ref = inputs.source, .path = clip});
  const svp::exec::CancellationToken cancellation;
  for (const svp::exec::TaskSpec* spec : {&workload.warm_up, &workload.batch}) {
    const svp::exec::TaskResult result = registry.execute(*spec, resolved, cancellation);
    require(result.status == svp::exec::TaskStatus::succeeded,
            std::string(spec == &workload.warm_up ? "the warm-up" : "the timed") +
                " calibration window is a clean result on a worker" +
                (result.error ? ": " + result.error->message : std::string()));
  }
  std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
  test_windows_are_spliced_before_the_fold();
  test_both_stages_split_together();
  test_window_count_scales_with_the_plan();
  test_outputs_reach_the_fold();
  test_progress_spans_windows_and_fold();
  test_tracking_calibration_sweep();
  test_runtime_probe();
  test_resume_follows_the_journal();
  test_calibration_windows_are_clean_on_a_worker();
#if defined(__APPLE__)
  test_calibration_records_by_kind();
#endif
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "All tracking window plan tests passed.\n";
  return 0;
}
