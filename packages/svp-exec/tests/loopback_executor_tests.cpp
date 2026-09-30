#include "scheduler_test_support.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/loopback_executor.hpp"
#include "svp/exec/scheduler.hpp"

#include <future>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

// Grace for worker processes in these tests: long enough for an idle worker
// to exit on SHUTDOWN, short enough that killing a busy one keeps the suite
// fast.
constexpr std::chrono::milliseconds kTestShutdownGrace{300};
// Cancellation must finish within the shutdown grace plus scheduling slack.
constexpr std::chrono::milliseconds kCancelDeadline{5'000};
// Long enough that only cancellation can end the task inside kCancelDeadline.
constexpr std::uint64_t kLongTaskMs = 60'000;
// How long the hang test waits for the scheduler to act on a clock advance
// before assuming a late heartbeat renewed the lease and advancing again.
constexpr std::chrono::milliseconds kExpiryObservationWait{200};

std::filesystem::path g_worker_path;

LoopbackExecutorOptions loopback_options(std::string id, std::size_t slots = 2) {
  return LoopbackExecutorOptions{.executor_id = std::move(id),
                                 .worker_executable = g_worker_path,
                                 .slots = slots,
                                 .shutdown_grace = kTestShutdownGrace};
}

ToyTask single(ToyFault fault, std::optional<std::filesystem::path> marker = std::nullopt,
               std::uint64_t sleep_ms = 0) {
  return ToyTask{.task_id = "task.toy.single",
                 .seed = 99,
                 .order_key = {.lane = "alpha", .ordinals = {0}},
                 .fault = fault,
                 .once_marker = std::move(marker),
                 .sleep_ms = sleep_ms};
}

BuildOutcome run(const TaskGraph& graph, std::vector<Executor*> executors,
                 ResultCommitSink& sink, const SchedulerPolicy& policy = test_policy(),
                 const AttemptObserver& observer = {}) {
  const SteadyClock clock;
  const CancellationToken cancellation;
  return Scheduler(policy, clock).run(graph, executors, sink, cancellation, observer);
}

void expect_failure(const BuildOutcome& outcome, BuildFailureKind kind,
                    std::string_view message) {
  if (!outcome.failure || outcome.failure->kind != kind) {
    throw std::runtime_error(
        std::string(message) + ": expected " + std::string(build_failure_kind_name(kind)) +
        ", got " + std::string(build_status_name(outcome.status)) +
        (outcome.failure ? " " + std::string(build_failure_kind_name(outcome.failure->kind)) +
                               " (" + outcome.failure->message + ")"
                         : std::string()));
  }
}

void test_normal_run() {
  const std::vector<ToyTask> tasks = two_lane_toy_tasks(6);
  const TaskGraph graph = make_toy_graph(tasks);
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run(graph, {&loopback}, sink);
  expect_succeeded(outcome, "loopback run");
  expect(sink.size() == graph.size(), "every task committed");
  expect(loopback.processes_started() == 1, "one worker process served the run");
  for (const ToyTask& task : tasks) {
    const CommittedResult* committed = sink.find(task.task_id);
    expect_equal(to_text(committed->payloads.front()),
                 toy_expected_output(task.task_id, task.seed), "loopback payload");
    expect(committed->result.execution.worker_session_id.starts_with("ws_test_worker_"),
           "result ran in the worker process");
  }
}

void test_in_process_and_loopback_agree() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(8));
  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 3});
  InMemoryResultCommitSink local_sink;
  expect_succeeded(run(graph, {&local}, local_sink), "in-process run");

  LoopbackExecutor loopback(loopback_options("loopback", 3));
  InMemoryResultCommitSink loopback_sink;
  expect_succeeded(run(graph, {&loopback}, loopback_sink), "loopback run");

  for (std::size_t index = 0; index < graph.size(); ++index) {
    const std::string& task_id = graph.node(index).spec.task_id;
    const CommittedResult* in_process = local_sink.find(task_id);
    const CommittedResult* remote = loopback_sink.find(task_id);
    expect(in_process->result.output_digest == remote->result.output_digest,
           "identical output digests for " + task_id);
    expect(in_process->payloads == remote->payloads, "identical payload bytes for " + task_id);
  }
  for (const std::string_view lane : {"alpha", "beta"}) {
    expect_equal(reduce_lane(graph, lane, local_sink.results()),
                 reduce_lane(graph, lane, loopback_sink.results()), "identical reductions");
  }
}

void test_child_crash_is_retried() {
  const TemporaryDirectory directory("svp-exec-loopback-crash");
  const TaskGraph graph = make_toy_graph({single(ToyFault::crash, directory.path / "crashed")});
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  EventLog log;
  const BuildOutcome outcome = run(graph, {&loopback}, sink, test_policy(), log.observer());
  expect_succeeded(outcome, "crash run");
  expect(outcome.stats.retries == 1, "one retry");
  expect(loopback.processes_started() == 2, "a fresh worker replaced the crashed one");
  expect(sink.find("task.toy.single")->result.attempt == 2, "attempt 2 committed");
  const auto events = log.events();
  expect(std::any_of(events.begin(), events.end(),
                     [](const AttemptEvent& event) {
                       return event.kind == AttemptEventKind::failed &&
                              event.detail.starts_with("executor_lost");
                     }),
         "crash reported as executor_lost");
}

void test_corrupted_payload_quarantines_and_retries_in_process() {
  const TaskGraph graph = make_toy_graph({single(ToyFault::corrupt_in_transit)});
  LoopbackExecutor loopback(loopback_options("loopback"));
  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run(graph, {&loopback, &local}, sink);
  expect_succeeded(outcome, "corruption run");
  expect(outcome.stats.invalid_results == 1, "one invalid result");
  expect(outcome.stats.quarantined_executors == std::vector<std::string>{"loopback"},
         "loopback quarantined");
  const CommittedResult* committed = sink.find("task.toy.single");
  expect(committed->executor_id == "in-process", "retried in-process");
  expect_equal(to_text(committed->payloads.front()),
               toy_expected_output("task.toy.single", 99), "correct bytes committed");
}

void test_hang_expires_lease_and_retries() {
  const TemporaryDirectory directory("svp-exec-loopback-hang");
  const std::filesystem::path marker = directory.path / "hung";
  const TaskGraph graph = make_toy_graph({single(ToyFault::hang, marker)});
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  EventLog log;
  ManualClock clock;
  const CancellationToken cancellation;
  std::vector<Executor*> executors{&loopback};
  auto outcome = std::async(std::launch::async, [&] {
    return Scheduler(test_policy(), clock).run(graph, executors, sink, cancellation, log.observer());
  });

  // Once the worker has stopped itself, move time past the lease. A heartbeat
  // already in flight may renew it, so repeat until the expiry is seen, but
  // only while attempt 1 is the only lease: the retry's lease must not expire.
  const auto deadline = std::chrono::steady_clock::now() + kTestWaitLimit;
  while (log.count(AttemptEventKind::expired) == 0) {
    expect(std::chrono::steady_clock::now() < deadline, "hang was never expired");
    if (std::filesystem::exists(marker) && log.count(AttemptEventKind::leased) == 1) {
      clock.advance(test_lease_length() + std::chrono::milliseconds(1));
      try {
        log.wait_until([](const auto& events) {
          return std::any_of(events.begin(), events.end(), [](const AttemptEvent& event) {
            return event.kind == AttemptEventKind::expired;
          });
        }, kExpiryObservationWait);
      } catch (const std::runtime_error&) {
        // Renewed by a late heartbeat; advance again.
      }
    }
    std::this_thread::sleep_for(kTestMaxIdleWait);
  }

  const BuildOutcome result = outcome.get();
  expect_succeeded(result, "hang run");
  expect(result.stats.leases_expired == 1, "one expiry");
  expect(loopback.processes_started() == 2, "the stalled worker was replaced");
  expect(sink.find("task.toy.single")->result.attempt == 2, "attempt 2 committed");
}

void test_cancel_stops_promptly() {
  const TaskGraph graph = make_toy_graph({single(ToyFault::none, std::nullopt, kLongTaskMs)});
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  EventLog log;
  const SteadyClock clock;
  CancellationToken cancellation;
  std::vector<Executor*> executors{&loopback};
  auto outcome = std::async(std::launch::async, [&] {
    return Scheduler(test_policy(), clock).run(graph, executors, sink, cancellation, log.observer());
  });
  log.wait_until([](const auto& events) { return !events.empty(); });
  const auto cancelled_at = std::chrono::steady_clock::now();
  cancellation.request();
  const BuildOutcome result = outcome.get();
  const auto elapsed = std::chrono::steady_clock::now() - cancelled_at;
  expect(result.status == BuildStatus::cancelled, "cancelled");
  expect(elapsed < kCancelDeadline, "cancel returned promptly");
  expect(sink.size() == 0, "nothing committed");
}

void test_nondeterministic_duplicate_is_an_incident() {
  const TaskGraph graph = make_toy_graph({single(ToyFault::nondeterministic)});
  LoopbackExecutor first(loopback_options("loopback-a", 1));
  LoopbackExecutor second(loopback_options("loopback-b", 1));
  InMemoryResultCommitSink sink;
  SchedulerPolicy policy = test_policy();
  policy.speculative_duplicates = true;
  const BuildOutcome outcome = run(graph, {&first, &second}, sink, policy);
  expect_failure(outcome, BuildFailureKind::determinism_incident, "nondeterminism");
  expect_equal(outcome.failure->task_id, "task.toy.single", "incident names the task");
  expect(outcome.incidents.size() == 1 && sink.size() == 1, "one committed, one conflicting");
  expect(outcome.incidents.front().conflicting.executor_id !=
             outcome.incidents.front().committed_executor_id,
         "the two results came from different workers");
}

void test_retries_exhausted_names_task() {
  const TaskGraph graph = make_toy_graph({single(ToyFault::fail_retryable)});
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run(graph, {&loopback}, sink);
  expect_failure(outcome, BuildFailureKind::retries_exhausted, "retries exhausted");
  expect_equal(outcome.failure->task_id, "task.toy.single", "failure names the task");
  expect(outcome.stats.attempts_started == kDefaultMaxAttempts, "every attempt was used");
  expect(sink.size() == 0, "no gap is filled silently");
}

void test_permanent_failure_is_not_retried() {
  const TaskGraph graph = make_toy_graph({single(ToyFault::fail_permanent)});
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run(graph, {&loopback}, sink);
  expect_failure(outcome, BuildFailureKind::permanent_task_failure, "permanent failure");
  expect(outcome.stats.attempts_started == 1, "a permanent failure is not retried");
}

// A worker task that keeps heartbeating but never finishes reaches its hard
// deadline: the scheduler sends CANCEL, the worker sets the attempt's token,
// the task stops, and the retry runs in the same (healthy) worker process.
void test_deadline_cancels_stuck_task_in_live_worker() {
  const TemporaryDirectory directory("svp-exec-loopback-deadline");
  const TaskGraph graph = make_toy_graph({ToyTask{.task_id = "task.toy.single",
                                                  .seed = 99,
                                                  .order_key = {.lane = "alpha", .ordinals = {0}},
                                                  .fault = ToyFault::stall,
                                                  .once_marker = directory.path / "stalled",
                                                  .est_seconds = 0}});
  // Leases far longer than the heartbeat cadence, so only the deadline ends
  // the stalled attempt; est_seconds 0 makes both floors apply.
  SchedulerPolicy policy = test_policy();
  policy.lease.lease_floor = std::chrono::milliseconds(1'000);
  policy.lease.attempt_deadline_floor = std::chrono::milliseconds(1'500);
  LoopbackExecutor loopback(loopback_options("loopback"));
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run(graph, {&loopback}, sink, policy);
  expect_succeeded(outcome, "stalled worker task retried");
  expect(outcome.stats.deadlines_exceeded == 1 && outcome.stats.leases_expired == 0,
         "ended by the deadline, not by lease expiry");
  expect(loopback.processes_started() == 1, "the live worker was cancelled, not killed");
  expect(sink.find("task.toy.single")->result.attempt == 2, "attempt 2 committed");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: svp-exec-loopback-executor-tests <svp-exec-test-worker>\n";
    return 2;
  }
  g_worker_path = argv[1];
  return run_tests(
      "svp-exec-loopback-executor-tests",
      {
          {"normal run", test_normal_run},
          {"in-process and loopback agree", test_in_process_and_loopback_agree},
          {"child crash is retried", test_child_crash_is_retried},
          {"corrupted payload quarantines and retries in-process",
           test_corrupted_payload_quarantines_and_retries_in_process},
          {"hang expires lease and retries", test_hang_expires_lease_and_retries},
          {"cancel stops promptly", test_cancel_stops_promptly},
          {"nondeterministic duplicate is an incident",
           test_nondeterministic_duplicate_is_an_incident},
          {"retries exhausted names task", test_retries_exhausted_names_task},
          {"permanent failure is not retried", test_permanent_failure_is_not_retried},
          {"deadline cancels stuck task in live worker",
           test_deadline_cancels_stuck_task_in_live_worker},
      });
}
