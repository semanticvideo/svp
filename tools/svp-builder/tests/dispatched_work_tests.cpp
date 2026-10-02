// The --distributed build's dispatched vision work (M4): running one stage's
// tasks across this Mac and the workers (run_subtasks), the dispatchers'
// hand-back rules, the capacity calibration graph of a dispatched type, and
// which vision work a build dispatches.

#include "calibration/capacity_sweep.hpp"
#include "engine/distributed_vision_work.hpp"
#include "engine/stage_output_access.hpp"
#include "engine/subtask_run.hpp"
#include "engine/vision_work_dispatch.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/tasks/embed_text_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"

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

  const char* cache = std::getenv("SVP_MODEL_CACHE_ROOT");
  if (cache == nullptr || *cache == '\0') {
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

}  // namespace

int main() {
  test_lost_worker_tasks_run_here();
  test_undeliverable_runs_fail();
  test_dispatchers_hand_back_unplanned_work();
  test_dispatcher_failures_fail_the_build();
  test_dispatched_outputs_are_released();
  test_dispatched_calibration_graph();
  test_planned_vision_work();
  std::cout << "dispatched work tests passed\n";
  return 0;
}
