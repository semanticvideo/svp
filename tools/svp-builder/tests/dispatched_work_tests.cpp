// The --distributed build's dispatched vision work (M4): running one stage's
// tasks across this Mac and the workers (run_subtasks), the dispatchers'
// hand-back rules, the leases workers declined, the capacity calibration
// graph of a dispatched type, which measurements `workers sync` takes ahead
// of builds, and which vision and audio work a build dispatches.

#include "calibration/audio_capacity_workloads.hpp"
#include "calibration/calibration_plan.hpp"
#include "calibration/capacity_sweep.hpp"
#include "calibration/dispatched_capacity_workloads.hpp"
#include "engine/declined_lease_tally.hpp"
#include "engine/distributed_audio_work.hpp"
#include "engine/distributed_vision_work.hpp"
#include "engine/stage_output_access.hpp"
#include "engine/subtask_run.hpp"
#include "engine/vision_work_dispatch.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/audio/tasks/asr_chunk_batch.hpp"
#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/tasks/embed_text_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <new>
#include <system_error>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace engine = svp::builder::engine;
namespace exec = svp::exec;
namespace vision = svp::vision;
namespace tasks = svp::vision::tasks;

constexpr const char* kEchoType = "test.echo";

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

std::vector<std::byte> bytes_of(const std::string& text) {
  std::vector<std::byte> bytes(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    bytes[index] = static_cast<std::byte>(text[index]);
  }
  return bytes;
}

// A task type whose one output is its own task ID: results are easy to
// match to the tasks they came from.
void register_echo(exec::TaskTypeRegistry& registry, engine::StageOutputAccess& outputs) {
  registry.register_type(exec::TaskTypeDefinition{
      .name = kEchoType,
      .version = 1,
      .validate_parameters = [](const nlohmann::json&) { return std::nullopt; },
      .execute = [&outputs](const exec::TaskSpec& spec, const exec::ResolvedInputs&,
                            const exec::CancellationToken&) {
        exec::TaskResult result;
        result.task_id = spec.task_id;
        result.attempt = 1;
        result.status = exec::TaskStatus::succeeded;
        result.execution.worker_session_id = "ws_unstamped";
        result.outputs = {outputs.put(bytes_of(spec.task_id), "text/plain", "echo")};
        result.output_digest = exec::compute_output_digest(result.outputs);
        return result;
      }});
}

std::vector<exec::TaskNode> echo_nodes(std::size_t count) {
  std::vector<exec::TaskNode> nodes;
  for (std::size_t index = 0; index < count; ++index) {
    exec::TaskSpec spec;
    spec.build_session_id = "bs_test";
    spec.task_id = "task.test.echo.items_" + std::to_string(1000 + index);
    spec.task_type = kEchoType;
    spec.task_type_version = 1;
    spec.parameters = {{"index", index}};
    spec.parameters_blake3 = exec::compute_parameters_blake3(spec.parameters);
    spec.cache_key = spec.parameters_blake3;
    spec.resources = {.est_peak_rss_mb = 0, .est_cpu_threads = 1, .est_seconds = 1};
    nodes.push_back({.spec = std::move(spec),
                     .order_key = {.lane = "test.echo", .ordinals = {index}}});
  }
  return nodes;
}

// A worker that is lost on every attempt it takes (its process killed or its
// network gone mid-task).
class LostWorker final : public exec::Executor {
 public:
  explicit LostWorker(std::string id) : id_(std::move(id)) {}
  std::string_view id() const override { return id_; }
  std::size_t slots() const override { return 2; }
  void start(exec::ExecutorEvents& events) override { events_ = &events; }
  void assign(const exec::TaskSpec&, const exec::Lease& lease) override {
    ++assigned;
    events_->attempt_failed(lease.lease_id, exec::AttemptFailureKind::executor_lost,
                            "worker process killed");
  }
  void cancel(std::string_view) override {}
  void lease_expired(std::string_view) override {}
  void stop() override {}

  std::atomic<std::size_t> assigned{0};

 private:
  std::string id_;
  exec::ExecutorEvents* events_ = nullptr;
};

class LostWorkers final : public svp::builder::DispatchedWorkerExecutors {
 public:
  std::vector<std::unique_ptr<exec::Executor>> make(std::string_view task_type) override {
    requested.insert(std::string(task_type));
    std::vector<std::unique_ptr<exec::Executor>> executors;
    executors.push_back(std::make_unique<LostWorker>("worker.lost"));
    return executors;
  }
  std::set<std::string> requested;
};

engine::VisionDispatchSetup echo_setup(std::size_t coordinator_slots) {
  engine::VisionDispatchSetup setup;
  setup.build_session_id = "bs_test";
  setup.capacity[kEchoType] = {.coordinator_slots = coordinator_slots, .seconds_per_item = 1.0};
  return setup;
}

// Every task commits exactly once and comes back in node order, even when a
// worker loses every attempt it takes: the attempts are retried on this
// Mac's slots.
void test_lost_worker_tasks_run_here() {
  exec::TaskTypeRegistry registry;
  engine::StageOutputAccess outputs;
  register_echo(registry, outputs);
  engine::VisionDispatchSetup setup = echo_setup(2);
  auto workers = std::make_shared<LostWorkers>();
  setup.workers = workers;
  std::vector<std::string> committed;
  const engine::SubtaskRunRequest request{
      .task_type = kEchoType,
      .nodes = echo_nodes(9),
      .on_committed = [&](const exec::TaskNode& node) { committed.push_back(node.spec.task_id); }};
  const std::vector<exec::CommittedResult> results =
      engine::run_subtasks(request, setup, registry, outputs);
  require(results.size() == 9 && committed.size() == 9, "every task commits once");
  for (std::size_t index = 0; index < results.size(); ++index) {
    require(results[index].result.task_id == request.nodes[index].spec.task_id &&
                results[index].payloads.size() == 1 &&
                results[index].payloads.front() == bytes_of(request.nodes[index].spec.task_id),
            "results come back in node order with their own outputs");
  }
  require(workers->requested == std::set<std::string>{kEchoType},
          "fresh worker executors are made for the stage's type");
}

// No executor for the type, or a task that fails everywhere: the stage gets
// DispatchedWorkError (the build fails), never partial outcomes.
void test_undeliverable_runs_fail() {
  exec::TaskTypeRegistry registry;
  engine::StageOutputAccess outputs;
  register_echo(registry, outputs);
  bool threw = false;
  try {
    (void)engine::run_subtasks({.task_type = "test.unknown", .nodes = echo_nodes(1), .on_committed = {}},
                               echo_setup(1), registry, outputs);
  } catch (const vision::DispatchedWorkError&) {
    threw = true;
  }
  require(threw, "a type this build does not dispatch is an error");

  engine::VisionDispatchSetup only_lost = echo_setup(0);
  only_lost.workers = std::make_shared<LostWorkers>();
  threw = false;
  try {
    (void)engine::run_subtasks({.task_type = kEchoType, .nodes = echo_nodes(2), .on_committed = {}},
                               only_lost, registry, outputs);
  } catch (const vision::DispatchedWorkError& error) {
    threw = std::string(error.what()).find("failed") != std::string::npos;
  }
  require(threw, "tasks that no executor completes fail the stage");
  require(engine::run_subtasks({.task_type = kEchoType, .nodes = {}, .on_committed = {}},
                               echo_setup(1), registry, outputs)
              .empty(),
          "no tasks is no run");
}

// A dispatcher hands the work back (nullopt) when its type is not dispatched
// in this build or the stage's model is not one the workers were given.
void test_dispatchers_hand_back_unplanned_work() {
  exec::TaskTypeRegistry registry;
  engine::StageOutputAccess outputs;
  auto setup = std::make_shared<engine::VisionDispatchSetup>();
  setup->build_session_id = "bs_test";
  setup->capacity[std::string(tasks::kEmbedTextBatchTaskType)] = {.coordinator_slots = 1,
                                                                   .seconds_per_item = 0.01};
  const svp::package::VisionWorkDispatch dispatch =
      engine::make_vision_work_dispatch(setup, registry, outputs);
  const vision::DispatchedModel model{.model_id = "model_nomic_embed_text_v1_5",
                                      .execution_provider = "cpu",
                                      .threads = {.intra_op = 2, .inter_op = 1}};
  require(!dispatch.text_embeddings({{.id = "a", .text = "alpha"}}, model, 768, {}).has_value(),
          "a model the workers were not given is embedded by the stage");
  require(!dispatch.evidence_crops({{.ordinal = 0, .seek_us = 0, .left = 0, .top = 0, .width = 8,
                                     .height = 8, .image_format = "jpeg", .jpeg_quality = 95}},
                                   vision::PpOcrOptions{}, {})
               .has_value(),
          "a type this build does not dispatch is done by the stage");
  require(!dispatch.depth_frames({}, model, {}).has_value() &&
              !dispatch.keyframe_embeddings({}, model, 768, {}).has_value(),
          "undispatched types hand their work back");
}

// A worker fleet whose executors cannot be made (the network or the system
// failed under it).
class FailingWorkers final : public svp::builder::DispatchedWorkerExecutors {
 public:
  explicit FailingWorkers(std::function<void()> fail) : fail_(std::move(fail)) {}
  std::vector<std::unique_ptr<exec::Executor>> make(std::string_view) override {
    fail_();
    return {};
  }

 private:
  std::function<void()> fail_;
};

// Whatever goes wrong while a dispatcher delivers (scheduler, executors,
// transport, memory), it leaves as DispatchedWorkError, the one exception
// the stages rethrow instead of recording as a blocker; its message is kept.
void test_dispatcher_failures_fail_the_build() {
  const std::vector<std::pair<std::string, std::function<void()>>> failures = {
      {"socket refused", [] {
         throw std::system_error(std::make_error_code(std::errc::connection_refused),
                                 "socket refused");
       }},
      {"graph broke", [] { throw exec::ExecError(exec::ExecErrorCode::invalid_value, "graph broke"); }},
      {"bad_alloc", [] { throw std::bad_alloc(); }},
  };
  for (const auto& [what, fail] : failures) {
    exec::TaskTypeRegistry registry;
    engine::StageOutputAccess outputs(engine::StageOutputRetention::release_after_read);
    auto setup = std::make_shared<engine::VisionDispatchSetup>();
    setup->build_session_id = "bs_test";
    setup->capacity[std::string(tasks::kEmbedTextBatchTaskType)] = {.coordinator_slots = 1,
                                                                     .seconds_per_item = 0.01};
    exec::Blake3Digest digest{};
    digest.fill(0x44);
    setup->model_refs = {{.model_id = "model_nomic_embed_text_v1_5",
                          .model_bundle_id = "model_nomic_embed_text_v1_5@test+blake3_" +
                                             exec::blake3_hex(digest).substr(0, 12),
                          .bundle_blake3 = digest}};
    setup->workers = std::make_shared<FailingWorkers>(fail);
    const svp::package::VisionWorkDispatch dispatch =
        engine::make_vision_work_dispatch(setup, registry, outputs);
    const vision::DispatchedModel model{.model_id = "model_nomic_embed_text_v1_5",
                                        .execution_provider = "cpu",
                                        .threads = {.intra_op = 2, .inter_op = 1}};
    bool converted = false;
    try {
      (void)dispatch.text_embeddings({{.id = "a", .text = "alpha"}}, model, 768, {});
    } catch (const vision::DispatchedWorkError& error) {
      converted = std::string(error.what()).find(what) != std::string::npos;
    } catch (...) {
    }
    require(converted, what + " leaves the dispatcher as DispatchedWorkError, message kept");
  }
}

// The coordinator's dispatched outputs are released once read back, so its
// memory does not grow with a stage's item count; the default store keeps
// them (whole-stage and OCR frame-batch tasks are unchanged).
void test_dispatched_outputs_are_released() {
  const std::vector<std::byte> bytes = bytes_of("vector");
  engine::StageOutputAccess releasing(engine::StageOutputRetention::release_after_read);
  const exec::ArtifactRef ref = releasing.put(bytes, "text/plain", "echo");
  (void)releasing.put(bytes, "text/plain", "echo");  // the same bytes from a second task
  exec::TaskResult result;
  result.task_id = "task.test.echo.items_1000";
  result.outputs = {ref};
  require(releasing.read_outputs(result).front() == bytes && releasing.stored_outputs() == 1,
          "bytes another task put stay until that task's read");
  require(releasing.read_outputs(result).front() == bytes && releasing.stored_outputs() == 0,
          "bytes go once every put was read");
  bool missing = false;
  try {
    (void)releasing.read_outputs(result);
  } catch (const std::runtime_error&) {
    missing = true;
  }
  require(missing, "released bytes are gone");

  engine::StageOutputAccess keeping;
  (void)keeping.put(bytes, "text/plain", "echo");
  (void)keeping.read_outputs(result);
  (void)keeping.read_outputs(result);
  require(keeping.stored_outputs() == 1, "the default store keeps outputs");

  // A whole dispatched run leaves nothing behind.
  exec::TaskTypeRegistry registry;
  engine::StageOutputAccess outputs(engine::StageOutputRetention::release_after_read);
  register_echo(registry, outputs);
  const auto results = engine::run_subtasks(
      {.task_type = kEchoType, .nodes = echo_nodes(12), .on_committed = {}}, echo_setup(3),
      registry, outputs);
  require(results.size() == 12 && outputs.stored_outputs() == 0,
          "a stage's dispatched outputs are released as they are committed");
}

// The sweep graph of a dispatched type: step k runs k warm-ups, then k timed
// copies of the batch after all of them, after the previous step.
void test_dispatched_calibration_graph() {
  exec::TaskSpec batch = echo_nodes(1).front().spec;
  exec::TaskSpec warm_up = echo_nodes(2).back().spec;
  const svp::builder::calibration::CapacityWorkload workload{
      .name = "ocr.crop_batch", .batch = batch, .warm_up = warm_up, .items_per_batch = 48};
  const auto graph = svp::builder::calibration::capacity_calibration_graph(workload, 3);
  require(graph.nodes.size() == 2 * (1 + 2 + 3), "k warm-ups and k timed tasks per step");
  require(graph.timed_step.size() == 6 && graph.timed_step.at(
              "task.calibration.ocr.crop_batch.slots_3.chain_2.batch_0") == 3,
          "timed tasks are named and stepped");
  const exec::TaskGraph built(graph.nodes);  // valid DAG, unique IDs and order keys
  require(built.size() == graph.nodes.size(), "the sweep is a valid graph");
  require(svp::builder::calibration::capacity_max_slots(8ULL << 30, 2ULL << 30, 10, 384) == 10 &&
              svp::builder::calibration::capacity_max_slots(8ULL << 30, 2ULL << 30, 10, 1024) == 6 &&
              svp::builder::calibration::capacity_max_slots(1ULL << 30, 2ULL << 30, 10, 384) == 1,
          "slots are bounded by memory per type and by CPUs, at least one");
}

// The vision work a build dispatches follows the model cache and the thread
// plan: no bundle or a thread count left to each Mac, no dispatch.
void test_planned_vision_work() {
  const std::filesystem::path empty =
      std::filesystem::temp_directory_path() / "svp-dispatch-empty-model-cache";
  std::filesystem::create_directories(empty);
  svp::models::ThreadPlan plan;
  plan.text_embedding = {.intra_op = 2, .inter_op = 1};
  plan.visual_entity_embedding = {.intra_op = 2, .inter_op = 1};
  plan.depth = {.intra_op = 2, .inter_op = 1};
  const svp::builder::DistributedVisionWork none = engine::plan_distributed_vision_work(empty, plan);
  require(none.evidence_crops && !none.text_embeddings && !none.keyframe_embeddings && !none.depth,
          "models missing from the cache are not dispatched");
  std::filesystem::remove_all(empty);

  // Needs a real model cache; CI points SVP_MODEL_CACHE_ROOT at a directory
  // with no model assets.
  const char* cache = std::getenv("SVP_MODEL_CACHE_ROOT");
  std::error_code error;
  if (cache == nullptr || *cache == '\0' || !std::filesystem::is_directory(cache, error) ||
      std::filesystem::is_empty(cache, error)) {
    return;
  }
  const svp::builder::DistributedVisionWork all = engine::plan_distributed_vision_work(cache, plan);
  require(all.text_embeddings && all.keyframe_embeddings && all.depth && all.embedding_dim > 0,
          "every cached model is dispatched");
  require(all.text_embeddings->model.threads == plan.text_embedding &&
              all.depth->model.threads == plan.depth,
          "dispatched models carry the thread plan's counts");
  plan.depth = {};  // left to each Mac
  require(!engine::plan_distributed_vision_work(cache, plan).depth,
          "host-chosen thread counts are not dispatched");
}


// A worker whose admission declines every lease it is offered (its memory
// is spoken for).
class DecliningWorker final : public exec::Executor {
 public:
  explicit DecliningWorker(std::string id) : id_(std::move(id)) {}
  std::string_view id() const override { return id_; }
  std::size_t slots() const override { return 2; }
  void start(exec::ExecutorEvents& events) override { events_ = &events; }
  void assign(const exec::TaskSpec&, const exec::Lease& lease) override {
    events_->attempt_rejected(lease.lease_id, "insufficient_memory",
                              "task needs 2304 MiB but the worker has 4000 MiB available");
  }
  void cancel(std::string_view) override {}
  void lease_expired(std::string_view) override {}
  void stop() override {}

 private:
  std::string id_;
  exec::ExecutorEvents* events_ = nullptr;
};

class DecliningWorkers final : public svp::builder::DispatchedWorkerExecutors {
 public:
  std::vector<std::unique_ptr<exec::Executor>> make(std::string_view) override {
    std::vector<std::unique_ptr<exec::Executor>> executors;
    executors.push_back(std::make_unique<DecliningWorker>("worker.declining"));
    return executors;
  }
};

// Collects what a block writes to stderr.
class CapturedStderr {
 public:
  CapturedStderr() : previous_(std::cerr.rdbuf(captured_.rdbuf())) {}
  ~CapturedStderr() { std::cerr.rdbuf(previous_); }
  std::string text() const { return captured_.str(); }

 private:
  std::ostringstream captured_;
  std::streambuf* previous_;
};

// Leases a worker declined are counted per executor and code and reported
// with the last reason; other attempt events are not declines.
void test_declined_lease_tally() {
  engine::DeclinedLeaseTally tally;
  require(tally.summary("asr.chunk_batch").empty(), "nothing declined, nothing reported");
  const auto event = [](exec::AttemptEventKind kind, std::string executor, std::string detail) {
    return exec::AttemptEvent{.kind = kind,
                              .task_id = "task.test",
                              .attempt = 1,
                              .executor_id = std::move(executor),
                              .lease_id = "lease_1",
                              .speculative = false,
                              .detail = std::move(detail)};
  };
  tally.observe(event(exec::AttemptEventKind::rejected, "worker.a",
                      "insufficient_memory: task needs 2304 MiB"));
  tally.observe(event(exec::AttemptEventKind::rejected, "worker.a",
                      "memory_pressure: the worker is under warning memory pressure"));
  tally.observe(event(exec::AttemptEventKind::rejected, "worker.a",
                      "insufficient_memory: task needs 2304 MiB, again"));
  tally.observe(event(exec::AttemptEventKind::failed, "worker.b", "lost"));
  tally.observe(event(exec::AttemptEventKind::committed, "worker.b", ""));
  require(tally.declined("worker.a") == 3 && tally.declined("worker.b") == 0,
          "only rejections count, per executor");
  const std::string summary = tally.summary("asr.chunk_batch");
  require(summary.find("svp-builder: asr.chunk_batch: worker.a declined 3 lease(s): "
                       "insufficient_memory 2, memory_pressure 1; last: insufficient_memory: "
                       "task needs 2304 MiB, again\n") == 0 &&
              summary.find("worker.b") == std::string::npos,
          "one line per declining executor, by code, with the last reason: " + summary);
}

// A stage whose worker declines every lease runs on this Mac, and says so:
// the summary names the worker, how many leases it declined, and why.
void test_declined_leases_are_reported() {
  exec::TaskTypeRegistry registry;
  engine::StageOutputAccess outputs;
  register_echo(registry, outputs);
  engine::VisionDispatchSetup setup = echo_setup(1);
  setup.workers = std::make_shared<DecliningWorkers>();
  setup.report = true;
  std::vector<exec::CommittedResult> results;
  std::string report;
  {
    const CapturedStderr captured;
    results = engine::run_subtasks({.task_type = kEchoType, .nodes = echo_nodes(4), .on_committed = {}},
                                   setup, registry, outputs);
    report = captured.text();
  }
  require(results.size() == 4, "every task commits on this Mac");
  for (const exec::CommittedResult& result : results) {
    require(result.executor_id == std::string("in-process.") + kEchoType,
            "the declining worker ran nothing");
  }
  require(report.find(std::string("svp-builder: ") + kEchoType +
                      ": worker.declining declined ") != std::string::npos &&
              report.find("insufficient_memory") != std::string::npos &&
              report.find("task needs 2304 MiB") != std::string::npos,
          "the stage reports the declined leases and their reason: " + report);
}

svp::exec::TaskModelRef test_model_ref(const std::string& id) {
  exec::Blake3Digest digest{};
  digest.fill(0x2a);
  return {.model_id = id,
          .model_bundle_id = id + "@test+blake3_" + exec::blake3_hex(digest).substr(0, 12),
          .bundle_blake3 = digest};
}

// `workers sync` measures every type a build dispatches, on the build's own
// terms: OCR, each vision type, tracking windows at the default quality, and
// each audio type, with diarize.window (which loads sherpa-onnx in this
// process) after every measurement that loads ONNX Runtime models.
void test_calibration_steps_cover_every_dispatched_type() {
  namespace calibration = svp::builder::calibration;
  namespace audio_tasks = svp::audio::tasks;
  require(calibration::calibrated_tracking_qualities() ==
              std::vector<vision::VisualTrackingQuality>{vision::kDefaultVisualTrackingQuality},
          "tracking is measured at the default build's quality");

  svp::builder::DistributedVisionWork vision;
  vision.evidence_crops = true;
  const vision::DispatchedModel model{.model_id = "model",
                                      .execution_provider = "cpu",
                                      .threads = {.intra_op = 2, .inter_op = 1}};
  vision.text_embeddings = svp::builder::DistributedOnnxWork{test_model_ref("text"), model};
  vision.keyframe_embeddings = svp::builder::DistributedOnnxWork{test_model_ref("frame"), model};
  vision.depth = svp::builder::DistributedOnnxWork{test_model_ref("depth"), model};
  svp::builder::DistributedAudioWork audio;
  audio.asr_model_refs = {test_model_ref("whisper"), test_model_ref("vad")};
  audio.diarization_model_ref = test_model_ref("sherpa");

  const std::vector<calibration::CalibrationStep> steps = calibration::calibration_steps(
      vision, audio, calibration::calibrated_tracking_qualities());
  std::vector<std::string> types;
  for (const calibration::CalibrationStep& step : steps) {
    types.push_back(step.task_type);
  }
  const auto covers = [&](const std::string& type) {
    return std::find(types.begin(), types.end(), type) != types.end();
  };
  require(steps.front().kind == calibration::CalibrationKind::ocr &&
              steps.front().task_type == tasks::kOcrFrameBatchTaskType,
          "OCR first");
  for (const std::string& type : calibration::dispatched_task_types(vision)) {
    require(covers(type), "every dispatched vision type is measured: " + type);
  }
  for (const std::string& type : calibration::audio_task_types(audio)) {
    require(covers(type), "every dispatched audio type is measured: " + type);
  }
  const auto tracking = std::find_if(steps.begin(), steps.end(), [](const auto& step) {
    return step.kind == calibration::CalibrationKind::tracking;
  });
  require(tracking != steps.end() && tracking->task_type == tasks::kTrackWindowTaskType &&
              tracking->tracking_quality == vision::kDefaultVisualTrackingQuality,
          "tracking windows are measured");
  require(types.size() == 1 + calibration::dispatched_task_types(vision).size() + 1 +
                              calibration::audio_task_types(audio).size(),
          "each type once");
  require(steps.back().task_type == audio_tasks::kDiarizeWindowTaskType,
          "diarize.window is measured last");

  audio.diarization_model_ref.reset();
  const std::vector<calibration::CalibrationStep> no_windows =
      calibration::calibration_steps(vision, audio, {});
  require(no_windows.back().task_type == audio_tasks::kAsrChunkBatchTaskType &&
              std::none_of(no_windows.begin(), no_windows.end(),
                           [](const auto& step) {
                             return step.kind == calibration::CalibrationKind::tracking ||
                                    step.task_type == audio_tasks::kDiarizeWindowTaskType;
                           }),
          "a type the build does not dispatch is not measured");
}

// The audio work `workers sync` measures is the audio work a build with
// audio dispatches: it follows the model cache and the thread plan only.
void test_planned_audio_models() {
  const std::filesystem::path empty =
      std::filesystem::temp_directory_path() / "svp-dispatch-empty-audio-model-cache";
  std::filesystem::create_directories(empty);
  svp::models::ThreadPlan plan;
  plan.whisper.decode = 2;
  plan.whisper.vad = 1;
  plan.forced_alignment = {.intra_op = 2, .inter_op = 1};
  plan.sherpa.segmentation = 1;
  plan.sherpa.embedding = 1;
  const svp::builder::DistributedAudioWork none = engine::plan_distributed_audio_models(empty, plan);
  require(none.asr_model_refs.empty() && !none.diarization_model_ref,
          "audio models missing from the cache are not dispatched");
  std::filesystem::remove_all(empty);
  require(engine::plan_distributed_audio_models({}, plan).asr_model_refs.empty(),
          "no model cache, no audio dispatch");

  // Needs a real model cache; CI points SVP_MODEL_CACHE_ROOT at a directory
  // with no model assets.
  const char* cache = std::getenv("SVP_MODEL_CACHE_ROOT");
  std::error_code error;
  if (cache == nullptr || *cache == '\0' || !std::filesystem::is_directory(cache, error) ||
      std::filesystem::is_empty(cache, error)) {
    return;
  }
  const svp::builder::DistributedAudioWork all = engine::plan_distributed_audio_models(cache, plan);
  require(all.asr_model_refs.size() >= 2 && all.diarization_model_ref,
          "every cached audio model is dispatched");
  plan.whisper.decode = 0;  // left to each Mac
  plan.sherpa.embedding = 0;
  const svp::builder::DistributedAudioWork host_chosen =
      engine::plan_distributed_audio_models(cache, plan);
  require(host_chosen.asr_model_refs.empty() && !host_chosen.diarization_model_ref,
          "host-chosen thread counts are not dispatched");
}

}  // namespace

int main() {
  test_lost_worker_tasks_run_here();
  test_undeliverable_runs_fail();
  test_dispatchers_hand_back_unplanned_work();
  test_dispatcher_failures_fail_the_build();
  test_dispatched_outputs_are_released();
  test_declined_lease_tally();
  test_declined_leases_are_reported();
  test_calibration_steps_cover_every_dispatched_type();
  test_planned_audio_models();
  test_dispatched_calibration_graph();
  test_planned_vision_work();
  std::cout << "dispatched work tests passed\n";
  return 0;
}
