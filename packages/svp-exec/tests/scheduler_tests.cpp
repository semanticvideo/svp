#include "scheduler_test_support.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/exec/task_attempt_runner.hpp"

#include <future>
#include <memory>
#include <set>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

using Responder = std::function<void(const TaskSpec&, const Lease&, ExecutorEvents&)>;

// Executor whose behaviour the test scripts: `respond` runs synchronously in
// assign() (on the scheduler thread); without it the executor stays silent
// and the test drives events() itself.
class ScriptedExecutor final : public Executor {
 public:
  ScriptedExecutor(std::string id, Responder respond = {},
                   LossQuarantine loss_quarantine = LossQuarantine::after_repeated_losses)
      : id_(std::move(id)), respond_(std::move(respond)), loss_quarantine_(loss_quarantine) {}

  std::string_view id() const override { return id_; }
  std::size_t slots() const override { return 1; }
  LossQuarantine loss_quarantine() const override { return loss_quarantine_; }
  void start(ExecutorEvents& events) override { events_ = &events; }
  void assign(const TaskSpec& spec, const Lease& lease) override {
    {
      const std::lock_guard lock(mutex_);
      assignments_.push_back(lease);
    }
    if (respond_) {
      respond_(spec, lease, *events_);
    }
  }
  void cancel(std::string_view) override {}
  void lease_expired(std::string_view lease_id) override {
    const std::lock_guard lock(mutex_);
    expired_.emplace_back(lease_id);
  }
  void stop() override {}

  ExecutorEvents& events() { return *events_; }
  std::vector<Lease> assignments() const {
    const std::lock_guard lock(mutex_);
    return assignments_;
  }
  std::vector<std::string> expired() const {
    const std::lock_guard lock(mutex_);
    return expired_;
  }

 private:
  std::string id_;
  Responder respond_;
  LossQuarantine loss_quarantine_;
  ExecutorEvents* events_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<Lease> assignments_;
  std::vector<std::string> expired_;
};

// A correct output for `spec` at `lease.attempt`, computed in-process.
AttemptOutput correct_output(ToyRuntime& runtime, const TaskSpec& spec, const Lease& lease) {
  return run_task_attempt(runtime.registry, runtime.store, spec,
                          AttemptContext{.attempt = lease.attempt, .worker_session_id = "ws_scripted"},
                          kNotCancelled);
}

TaskGraph single_task_graph() {
  return make_toy_graph({ToyTask{.task_id = "task.toy.only",
                                 .seed = 42,
                                 .order_key = {.lane = "alpha", .ordinals = {0}}}});
}

BuildOutcome run_sync(const TaskGraph& graph, std::vector<Executor*> executors,
                      ResultCommitSink& sink, const SchedulerPolicy& policy = test_policy(),
                      const AttemptObserver& observer = {}) {
  const SteadyClock clock;
  const CancellationToken cancellation;
  return Scheduler(policy, clock).run(graph, executors, sink, cancellation, observer);
}

void expect_payload_is_correct(const CommittedResult& committed, std::uint64_t seed) {
  expect(committed.payloads.size() == 1, "one payload");
  expect_equal(to_text(committed.payloads.front()),
               toy_expected_output(committed.result.task_id, seed), "committed payload");
}

void test_dispatch_order() {
  auto task = [](std::string id, std::vector<std::string> deps, std::uint64_t ordinal,
                 std::uint64_t est) {
    return ToyTask{.task_id = std::move(id),
                   .depends_on = std::move(deps),
                   .seed = ordinal,
                   .order_key = {.lane = "lane", .ordinals = {ordinal}},
                   .est_seconds = est};
  };
  // Critical paths: c 11 (c -> d), d 10, b 5, a 1, e 1 (a before e by key).
  const TaskGraph graph = make_toy_graph({task("t.a", {}, 3, 1), task("t.b", {}, 2, 5),
                                          task("t.c", {}, 1, 1), task("t.d", {"t.c"}, 0, 10),
                                          task("t.e", {}, 4, 1)});
  ToyRuntime runtime;
  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  EventLog log;
  expect_succeeded(run_sync(graph, {&executor}, sink, test_policy(), log.observer()),
                   "dispatch order run");
  std::vector<std::string> leased;
  for (const AttemptEvent& event : log.events()) {
    if (event.kind == AttemptEventKind::leased) {
      leased.push_back(event.task_id);
    }
  }
  expect(leased == std::vector<std::string>{"t.c", "t.d", "t.b", "t.a", "t.e"},
         "critical path first, then canonical order key");
}

void test_random_completion_orders() {
  constexpr std::size_t kRepetitions = 50;
  constexpr std::size_t kThreads = 4;
  constexpr std::chrono::microseconds kMaxJitter{2'000};
  const std::vector<ToyTask> tasks = two_lane_toy_tasks(12);
  const TaskGraph graph = make_toy_graph(tasks);
  ToyRuntime runtime(ToyTaskOptions{.max_jitter = kMaxJitter});

  std::string alpha;
  std::string beta;
  std::set<std::vector<std::string>> commit_orders;
  for (std::size_t run = 0; run < kRepetitions; ++run) {
    InProcessExecutor executor(runtime.registry, runtime.store, {.threads = kThreads});
    InMemoryResultCommitSink sink;
    EventLog log;
    expect_succeeded(run_sync(graph, {&executor}, sink, test_policy(), log.observer()),
                     "jittered run");
    expect(sink.size() == graph.size(), "every task committed");
    std::vector<std::string> order;
    for (const AttemptEvent& event : log.events()) {
      if (event.kind == AttemptEventKind::committed) {
        order.push_back(event.task_id);
      }
    }
    commit_orders.insert(order);
    const std::vector<CommittedResult> results = sink.results();
    if (run == 0) {
      alpha = reduce_lane(graph, "alpha", results);
      beta = reduce_lane(graph, "beta", results);
    }
    expect_equal(reduce_lane(graph, "alpha", results), alpha, "alpha reduction");
    expect_equal(reduce_lane(graph, "beta", results), beta, "beta reduction");
  }
  expect(commit_orders.size() > 1, "jitter produced more than one completion order");
}

// Output must not depend on how many executors ran the tasks, or which ones:
// one executor alone and several of mixed capacity reduce to the same bytes.
void test_executor_count_does_not_change_output() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(10));
  ToyRuntime runtime;
  const auto reduce_with = [&](const std::vector<std::size_t>& thread_counts) {
    std::vector<std::unique_ptr<InProcessExecutor>> owned;
    std::vector<Executor*> executors;
    for (std::size_t index = 0; index < thread_counts.size(); ++index) {
      owned.push_back(std::make_unique<InProcessExecutor>(
          runtime.registry, runtime.store,
          InProcessExecutorOptions{.executor_id = "exec_" + std::to_string(index),
                                   .threads = thread_counts[index]}));
      executors.push_back(owned.back().get());
    }
    InMemoryResultCommitSink sink;
    expect_succeeded(run_sync(graph, executors, sink), "executor mix run");
    const std::vector<CommittedResult> results = sink.results();
    return reduce_lane(graph, "alpha", results) + reduce_lane(graph, "beta", results);
  };
  const std::string single = reduce_with({1});
  expect_equal(reduce_with({3}), single, "one wide executor");
  expect_equal(reduce_with({1, 2, 3}), single, "three executors of mixed capacity");
}

void test_lease_expiry_retries_elsewhere() {
  const TaskGraph graph = single_task_graph();
  ToyRuntime runtime;
  ScriptedExecutor silent("silent");
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  EventLog log;
  ManualClock clock;
  const CancellationToken cancellation;
  std::vector<Executor*> executors{&silent, &local};

  auto outcome = std::async(std::launch::async, [&] {
    return Scheduler(test_policy(), clock).run(graph, executors, sink, cancellation, log.observer());
  });
  log.wait_until([](const auto& events) { return !events.empty(); });
  expect(silent.assignments().size() == 1, "silent executor took the task first");
  clock.advance(test_lease_length());

  const BuildOutcome result = outcome.get();
  expect_succeeded(result, "expiry run");
  expect(result.stats.leases_expired == 1 && result.stats.retries == 1, "one expiry, one retry");
  expect(silent.expired() == std::vector<std::string>{silent.assignments()[0].lease_id},
         "executor told about the expiry");
  expect(sink.find("task.toy.only")->executor_id == "in-process", "retry ran elsewhere");
  expect(sink.find("task.toy.only")->result.attempt == 2, "retry is attempt 2");
}

void test_heartbeats_renew_lease() {
  const TaskGraph graph = single_task_graph();
  ToyRuntime runtime;
  ScriptedExecutor worker("worker");
  InMemoryResultCommitSink sink;
  EventLog log;
  ManualClock clock;
  const CancellationToken cancellation;
  std::vector<Executor*> executors{&worker};

  auto outcome = std::async(std::launch::async, [&] {
    return Scheduler(test_policy(), clock).run(graph, executors, sink, cancellation, log.observer());
  });
  log.wait_until([](const auto& events) { return !events.empty(); });
  const Lease lease = worker.assignments().front();
  const auto almost = test_lease_length() - std::chrono::milliseconds(1);
  for (int renewal = 0; renewal < 3; ++renewal) {
    clock.advance(almost);
    worker.events().lease_heartbeat(lease.lease_id);
    // Let the scheduler apply the heartbeat before time moves again.
    std::this_thread::sleep_for(kTestMaxIdleWait * 4);
  }
  worker.events().attempt_finished(lease.lease_id,
                                   correct_output(runtime, graph.node(0).spec, lease));
  const BuildOutcome result = outcome.get();
  expect_succeeded(result, "heartbeat run");
  expect(result.stats.leases_expired == 0, "heartbeats kept the lease alive");
  expect(result.stats.attempts_started == 1, "one attempt");
}

void test_invalid_result_quarantines_executor() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(2));
  ToyRuntime runtime;
  ScriptedExecutor liar("liar", [&](const TaskSpec& spec, const Lease& lease,
                                    ExecutorEvents& events) {
    AttemptOutput output = correct_output(runtime, spec, lease);
    output.payloads.front().front() ^= std::byte{0x20};
    events.attempt_finished(lease.lease_id, std::move(output));
  });
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run_sync(graph, {&liar, &local}, sink);
  expect_succeeded(outcome, "run with a lying executor");
  expect(outcome.stats.invalid_results == 1, "one invalid result");
  expect(outcome.stats.quarantined_executors == std::vector<std::string>{"liar"}, "quarantined");
  expect(liar.assignments().size() == 1, "no work after quarantine");
  for (const ToyTask& task : two_lane_toy_tasks(2)) {
    const CommittedResult* committed = sink.find(task.task_id);
    expect(committed != nullptr && committed->executor_id == "in-process",
           "every commit came from the honest executor");
    expect_payload_is_correct(*committed, task.seed);
  }
}

void test_retries_exhausted_fails_build() {
  const TaskGraph graph = single_task_graph();
  ToyRuntime runtime;
  ScriptedExecutor flaky("flaky", [&](const TaskSpec& spec, const Lease& lease,
                                      ExecutorEvents& events) {
    AttemptOutput output = correct_output(runtime, spec, lease);
    output.result.status = TaskStatus::failed;
    output.result.outputs.clear();
    output.result.output_digest = compute_output_digest({});
    output.result.error = TaskError{.code = "flaky", .message = "try again", .retryable = true};
    output.payloads.clear();
    events.attempt_finished(lease.lease_id, std::move(output));
  });
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run_sync(graph, {&flaky}, sink);
  expect(outcome.status == BuildStatus::failed && outcome.failure &&
             outcome.failure->kind == BuildFailureKind::retries_exhausted,
         "retries exhausted");
  expect_equal(outcome.failure->task_id, "task.toy.only", "failure names the task");
  expect(outcome.failure->message.find("task.toy.only") != std::string::npos,
         "message names the task");
  expect(flaky.assignments().size() == kDefaultMaxAttempts, "max_attempts attempts");
  expect(sink.size() == 0, "a failed result is never committed");
}

void test_permanent_failure_fails_build() {
  std::vector<ToyTask> tasks{
      ToyTask{.task_id = "t.bad", .seed = 1, .order_key = {.lane = "l", .ordinals = {0}}},
      ToyTask{.task_id = "t.after", .depends_on = {"t.bad"}, .seed = 2,
              .order_key = {.lane = "l", .ordinals = {1}}},
  };
  std::vector<TaskNode> nodes{make_toy_node(tasks[0]), make_toy_node(tasks[1])};
  nodes[0].spec.parameters = nlohmann::json{{"seed", "not a number"}};
  nodes[0].spec.parameters_blake3 = compute_parameters_blake3(nodes[0].spec.parameters);
  const TaskGraph graph(std::move(nodes));

  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  EventLog log;
  const BuildOutcome outcome = run_sync(graph, {&local}, sink, test_policy(), log.observer());
  expect(outcome.failure && outcome.failure->kind == BuildFailureKind::permanent_task_failure,
         "permanent failure");
  expect_equal(outcome.failure->task_id, "t.bad", "failure names the task");
  expect(sink.size() == 0 && log.count(AttemptEventKind::leased) == 1,
         "nothing committed and the dependent never ran");
}

void test_speculative_duplicate_is_discarded() {
  const TaskGraph graph = single_task_graph();
  ToyRuntime runtime;
  InProcessExecutor first(runtime.registry, runtime.store, {.executor_id = "first", .threads = 1});
  InProcessExecutor second(runtime.registry, runtime.store,
                           {.executor_id = "second", .threads = 1});
  InMemoryResultCommitSink sink;
  SchedulerPolicy policy = test_policy();
  policy.speculative_duplicates = true;
  const BuildOutcome outcome = run_sync(graph, {&first, &second}, sink, policy);
  expect_succeeded(outcome, "speculative run");
  expect(outcome.stats.speculative_attempts == 1 && outcome.stats.duplicates_discarded == 1,
         "duplicate ran and was discarded");
  expect(sink.size() == 1, "committed once");
}

void test_determinism_incident_fails_build() {
  const TaskGraph graph = single_task_graph();
  ToyRuntime runtime;
  ScriptedExecutor honest("honest", [&](const TaskSpec& spec, const Lease& lease,
                                        ExecutorEvents& events) {
    events.attempt_finished(lease.lease_id, correct_output(runtime, spec, lease));
  });
  ScriptedExecutor drifting("drifting", [&](const TaskSpec& spec, const Lease& lease,
                                            ExecutorEvents& events) {
    TaskSpec other = spec;
    other.parameters["seed"] = std::uint64_t{43};
    other.parameters_blake3 = compute_parameters_blake3(other.parameters);
    events.attempt_finished(lease.lease_id, correct_output(runtime, other, lease));
  });
  InMemoryResultCommitSink sink;
  SchedulerPolicy policy = test_policy();
  policy.speculative_duplicates = true;
  EventLog log;
  const BuildOutcome outcome = run_sync(graph, {&honest, &drifting}, sink, policy, log.observer());
  std::string trace;
  for (const AttemptEvent& event : log.events()) {
    trace += std::string(attempt_event_kind_name(event.kind)) + "(" + event.executor_id + " " +
             event.detail + ") ";
  }
  expect(outcome.failure && outcome.failure->kind == BuildFailureKind::determinism_incident,
         "incident fails the build: " + trace);
  expect(outcome.incidents.size() == 1, "one incident");
  const DeterminismIncident& incident = outcome.incidents.front();
  expect(incident.committed_executor_id == "honest" &&
             incident.conflicting.executor_id == "drifting",
         "both results kept");
  expect(incident.committed_output_digest != incident.conflicting.result.output_digest,
         "digests differ");
  expect(sink.find("task.toy.only")->executor_id == "honest", "first verified result committed");
}

void test_cancellation_between_tasks() {
  std::vector<ToyTask> chain;
  for (std::uint64_t index = 0; index < 10; ++index) {
    ToyTask task{.task_id = "t.chain_" + std::to_string(index),
                 .seed = index,
                 .order_key = {.lane = "chain", .ordinals = {index}}};
    if (index > 0) {
      task.depends_on = {"t.chain_" + std::to_string(index - 1)};
    }
    chain.push_back(task);
  }
  const TaskGraph graph = make_toy_graph(chain);
  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  CancellationToken cancellation;
  std::size_t leased = 0;
  const SteadyClock clock;
  std::vector<Executor*> executors{&local};
  const BuildOutcome outcome = Scheduler(test_policy(), clock).run(
      graph, executors, sink, cancellation, [&](const AttemptEvent& event) {
        leased += event.kind == AttemptEventKind::leased ? 1 : 0;
        if (event.kind == AttemptEventKind::committed) {
          cancellation.request();
        }
      });
  expect(outcome.status == BuildStatus::cancelled, "cancelled");
  expect(sink.size() == 1 && leased == 1, "no task leased after cancellation");
}

void test_commit_failure_fails_build() {
  class FailingSink final : public ResultCommitSink {
   public:
    void commit(const TaskSpec&, const CommittedResult&) override {
      throw std::runtime_error("journal disk full");
    }
  };
  const TaskGraph graph = single_task_graph();
  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  FailingSink sink;
  const BuildOutcome outcome = run_sync(graph, {&local}, sink);
  expect(outcome.failure && outcome.failure->kind == BuildFailureKind::commit_failed &&
             outcome.failure->message.find("journal disk full") != std::string::npos,
         "commit failure fails the build");
}

void test_no_usable_executor() {
  const TaskGraph graph = single_task_graph();
  ScriptedExecutor garbage("garbage", [](const TaskSpec&, const Lease& lease,
                                         ExecutorEvents& events) {
    events.attempt_failed(lease.lease_id, AttemptFailureKind::invalid_result, "bad frame");
  });
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run_sync(graph, {&garbage}, sink);
  expect(outcome.failure && outcome.failure->kind == BuildFailureKind::no_usable_executor,
         "all executors quarantined");
}

void test_repeated_losses_quarantine_executor() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(4));
  ToyRuntime runtime;
  ScriptedExecutor lossy("lossy", [](const TaskSpec&, const Lease& lease,
                                     ExecutorEvents& events) {
    events.attempt_failed(lease.lease_id, AttemptFailureKind::executor_lost, "connection reset");
  });
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run_sync(graph, {&lossy, &local}, sink);
  expect_succeeded(outcome, "run with a lossy executor");
  expect(lossy.assignments().size() == kDefaultQuarantineAfterExecutorFailures,
         "lossy executor quarantined after the threshold");
  expect(outcome.stats.quarantined_executors == std::vector<std::string>{"lossy"},
         "quarantine recorded");
  expect(sink.size() == graph.size(), "all tasks committed");
}

// Loses attempt 1 of every task, then answers correctly: each task needs one
// retry, and the executor collects one loss per task.
Responder lose_first_attempts(ToyRuntime& runtime) {
  return [&runtime](const TaskSpec& spec, const Lease& lease, ExecutorEvents& events) {
    if (lease.attempt == 1) {
      events.attempt_failed(lease.lease_id, AttemptFailureKind::executor_lost, "worker slept");
    } else {
      events.attempt_finished(lease.lease_id, correct_output(runtime, spec, lease));
    }
  };
}

// The only executor collects more transient losses than the quarantine
// threshold. Declared LossQuarantine::never, it keeps working and the build
// succeeds; per-task max_attempts still bounds each task.
void test_losses_never_quarantine_a_never_executor() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(2));
  expect(graph.size() > kDefaultQuarantineAfterExecutorFailures,
         "more losses than the quarantine threshold");
  ToyRuntime runtime;
  ScriptedExecutor local("local", lose_first_attempts(runtime), LossQuarantine::never);
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run_sync(graph, {&local}, sink);
  expect_succeeded(outcome, "single never-quarantined executor with transient losses");
  expect(outcome.stats.quarantined_executors.empty(), "not quarantined for losses");
  expect(outcome.stats.retries == graph.size(), "one retry per task");

  // The same losses on a remote-style executor quarantine it, and with no
  // other executor the build cannot finish.
  ScriptedExecutor remote("remote", lose_first_attempts(runtime));
  InMemoryResultCommitSink remote_sink;
  const BuildOutcome remote_outcome = run_sync(graph, {&remote}, remote_sink);
  expect(remote_outcome.failure &&
             remote_outcome.failure->kind == BuildFailureKind::no_usable_executor,
         "after_repeated_losses quarantines after the threshold");
}

// Invalid results still quarantine a LossQuarantine::never executor at once.
void test_invalid_result_quarantines_a_never_executor() {
  const TaskGraph graph = single_task_graph();
  ScriptedExecutor local("local", [](const TaskSpec&, const Lease& lease, ExecutorEvents& events) {
    events.attempt_failed(lease.lease_id, AttemptFailureKind::invalid_result, "bad payload");
  }, LossQuarantine::never);
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run_sync(graph, {&local}, sink);
  expect(outcome.stats.quarantined_executors == std::vector<std::string>{"local"},
         "an invalid result quarantines regardless of loss policy");
}

void test_in_process_executor_is_never_quarantined_for_losses() {
  ToyRuntime runtime;
  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 1});
  expect(executor.loss_quarantine() == LossQuarantine::never, "in-process loss policy");
}

// Real-time policy for deadline tests: leases far longer than the heartbeat
// cadence (so only the deadline can end an attempt), and a deadline short
// enough to keep the test quick. Tasks use est_seconds 0, so both floors apply.
SchedulerPolicy deadline_policy() {
  SchedulerPolicy policy = test_policy();
  policy.lease.lease_floor = std::chrono::milliseconds(1'000);
  policy.lease.attempt_deadline_floor = std::chrono::milliseconds(1'500);
  return policy;
}

// A task that is alive (heartbeating) but never finishes is cancelled at its
// hard deadline, counted as lost, and retried; the retry commits.
void test_deadline_cancels_a_live_stuck_attempt() {
  const TemporaryDirectory directory("svp-exec-scheduler-deadline");
  ToyTask stuck{.task_id = "task.toy.stuck",
                .seed = 11,
                .order_key = {.lane = "alpha", .ordinals = {0}},
                .fault = ToyFault::stall,
                .once_marker = directory.path / "stalled",
                .est_seconds = 0};
  const TaskGraph graph = make_toy_graph({stuck});
  ToyRuntime runtime(ToyTaskOptions{.faults = ToyFaults::honoured});
  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  EventLog log;
  const BuildOutcome outcome = run_sync(graph, {&executor}, sink, deadline_policy(), log.observer());
  expect_succeeded(outcome, "stuck attempt retried");
  expect(outcome.stats.deadlines_exceeded == 1, "one deadline");
  expect(outcome.stats.leases_expired == 0, "heartbeats kept the lease alive");
  expect(log.count(AttemptEventKind::deadline_exceeded) == 1, "deadline event");
  expect(outcome.stats.quarantined_executors.empty(), "the in-process executor stays usable");
  const CommittedResult* committed = sink.find("task.toy.stuck");
  expect(committed != nullptr && committed->result.attempt == 2, "attempt 2 committed");
  expect_payload_is_correct(*committed, 11);
}

}  // namespace

int main() {
  return run_tests("svp-exec-scheduler-tests",
                   {
                       {"dispatch order", test_dispatch_order},
                       {"random completion orders", test_random_completion_orders},
                       {"executor count does not change output",
                        test_executor_count_does_not_change_output},
                       {"lease expiry retries elsewhere", test_lease_expiry_retries_elsewhere},
                       {"heartbeats renew lease", test_heartbeats_renew_lease},
                       {"invalid result quarantines executor",
                        test_invalid_result_quarantines_executor},
                       {"retries exhausted fails build", test_retries_exhausted_fails_build},
                       {"permanent failure fails build", test_permanent_failure_fails_build},
                       {"speculative duplicate is discarded",
                        test_speculative_duplicate_is_discarded},
                       {"determinism incident fails build",
                        test_determinism_incident_fails_build},
                       {"cancellation between tasks", test_cancellation_between_tasks},
                       {"commit failure fails build", test_commit_failure_fails_build},
                       {"no usable executor", test_no_usable_executor},
                       {"repeated losses quarantine executor",
                        test_repeated_losses_quarantine_executor},
                       {"losses never quarantine a never executor",
                        test_losses_never_quarantine_a_never_executor},
                       {"invalid result quarantines a never executor",
                        test_invalid_result_quarantines_a_never_executor},
                       {"in-process executor is never quarantined for losses",
                        test_in_process_executor_is_never_quarantined_for_losses},
                       {"deadline cancels a live stuck attempt",
                        test_deadline_cancels_a_live_stuck_attempt},
                   });
}
