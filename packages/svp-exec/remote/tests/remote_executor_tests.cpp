// RemoteExecutor against svp-exec-test-worker --listen processes on this Mac:
// the same scheduler behaviour LoopbackExecutor proves over a socket pair,
// now over TLS-PSK connections found through Bonjour by pairing id.

#include "listening_worker_process.hpp"
#include "pairing_test_support.hpp"
#include "scheduler_test_support.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/loopback_executor.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/scheduler.hpp"

#include <future>
#include <iostream>
#include <set>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;
using svp::exec::remote::test::ListeningWorkerProcess;
using remote::PairingKey;
using remote::RemoteExecutor;
using remote::RemoteExecutorOptions;
namespace remote_test = svp::exec::remote::test;

// Grace for sessions in these tests: long enough for an idle worker session
// to end on SHUTDOWN, short enough that tearing down a busy one keeps the
// suite fast.
constexpr std::chrono::milliseconds kTestShutdownGrace{300};
// Cancellation must finish within the shutdown grace plus scheduling slack.
constexpr std::chrono::milliseconds kCancelDeadline{5'000};
// Long enough that only cancellation or a kill can end the task in a test.
constexpr std::uint64_t kLongTaskMs = 60'000;
// Long enough that the worker is still running the task when the test
// kills it, short enough that the retry keeps the suite fast.
constexpr std::uint64_t kKillableTaskMs = 1'500;
// How long the hang test waits for the scheduler to act on a clock advance
// before assuming a late heartbeat renewed the lease and advancing again.
constexpr std::chrono::milliseconds kExpiryObservationWait{200};
// Tasks that must overlap so two single-slot workers both get work.
constexpr std::uint64_t kOverlapTaskMs = 50;
// Discovery bound for pairings nobody advertises; keeps that test fast.
constexpr std::chrono::milliseconds kShortDiscovery{500};
// How long the killed worker stays down before it is restarted: several
// reconnect pauses, well inside the reconnect window, so the session must
// rediscover the restarted worker rather than find it on its first try.
constexpr std::chrono::milliseconds kWorkerDowntime{4 * remote::kDefaultReconnectPause};

std::filesystem::path g_worker_path;

RemoteExecutorOptions remote_options(std::string id, const PairingKey& key,
                                     std::size_t slots = 2) {
  RemoteExecutorOptions options;
  options.executor_id = std::move(id);
  options.connector.pairing = key;
  options.slots = slots;
  options.shutdown_grace = kTestShutdownGrace;
  return options;
}

struct Workers {
  TemporaryDirectory directory{"svp-exec-remote"};

  std::unique_ptr<ListeningWorkerProcess> start(std::string_view label) {
    return start(remote_test::random_pairing(label));
  }
  std::unique_ptr<ListeningWorkerProcess> start(PairingKey key) {
    return std::make_unique<ListeningWorkerProcess>(g_worker_path, std::move(key),
                                                    directory.path);
  }
};

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

bool has_failure(const std::vector<AttemptEvent>& events, std::string_view prefix,
                 std::string_view fragment = {}) {
  return std::any_of(events.begin(), events.end(), [&](const AttemptEvent& event) {
    return event.kind == AttemptEventKind::failed && event.detail.starts_with(prefix) &&
           event.detail.find(fragment) != std::string::npos;
  });
}

void expect_identical(const TaskGraph& graph, const InMemoryResultCommitSink& expected,
                      const InMemoryResultCommitSink& actual, std::string_view label) {
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const std::string& task_id = graph.node(index).spec.task_id;
    const CommittedResult* want = expected.find(task_id);
    const CommittedResult* got = actual.find(task_id);
    expect(want && got, std::string(label) + ": both committed " + task_id);
    expect(want->result.output_digest == got->result.output_digest,
           std::string(label) + ": identical output digest for " + task_id);
    expect(want->payloads == got->payloads,
           std::string(label) + ": identical payload bytes for " + task_id);
  }
  for (const std::string_view lane : {"alpha", "beta"}) {
    expect_equal(reduce_lane(graph, lane, expected.results()),
                 reduce_lane(graph, lane, actual.results()),
                 std::string(label) + ": identical reduction");
  }
}

void test_normal_run() {
  Workers workers;
  const auto worker = workers.start("svp-remote-exec");
  const std::vector<ToyTask> tasks = two_lane_toy_tasks(6);
  const TaskGraph graph = make_toy_graph(tasks);
  RemoteExecutor remote(remote_options("remote", worker->key()));
  InMemoryResultCommitSink sink;
  expect_succeeded(run(graph, {&remote}, sink), "remote run");
  expect(sink.size() == graph.size(), "every task committed");
  expect(remote.connections_opened() == 1, "one connection served the run");
  for (const ToyTask& task : tasks) {
    const CommittedResult* committed = sink.find(task.task_id);
    expect_equal(to_text(committed->payloads.front()),
                 toy_expected_output(task.task_id, task.seed), "remote payload");
    expect(committed->result.execution.worker_session_id.starts_with(
               "ws_test_worker_" + std::to_string(worker->pid()) + "_"),
           "result ran in the listening worker");
  }
  const auto route = remote.last_route();
  expect(route.has_value(), "route recorded");
  std::cout << "  route: " << route->route.interface_name << " "
            << remote::route_medium_name(route->route.medium) << " handshake "
            << route->handshake.count() << " us of " << route->probes.size()
            << " candidates\n";
}

void test_matches_in_process_and_loopback() {
  Workers workers;
  const auto worker = workers.start("svp-remote-exec");
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(8));

  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 3});
  InMemoryResultCommitSink local_sink;
  expect_succeeded(run(graph, {&local}, local_sink), "in-process run");

  LoopbackExecutor loopback(LoopbackExecutorOptions{.executor_id = "loopback",
                                                    .worker_executable = g_worker_path,
                                                    .slots = 3,
                                                    .shutdown_grace = kTestShutdownGrace});
  InMemoryResultCommitSink loopback_sink;
  expect_succeeded(run(graph, {&loopback}, loopback_sink), "loopback run");

  RemoteExecutor remote(remote_options("remote", worker->key(), 3));
  InMemoryResultCommitSink remote_sink;
  expect_succeeded(run(graph, {&remote}, remote_sink), "remote run");

  expect_identical(graph, local_sink, remote_sink, "in-process vs remote");
  expect_identical(graph, loopback_sink, remote_sink, "loopback vs remote");
}

void test_two_workers_match_one() {
  Workers workers;
  const auto first = workers.start("svp-remote-exec-a");
  const auto second = workers.start("svp-remote-exec-b");
  std::vector<ToyTask> tasks = two_lane_toy_tasks(6);
  for (ToyTask& task : tasks) {
    task.sleep_ms = kOverlapTaskMs;
  }
  const TaskGraph graph = make_toy_graph(tasks);

  RemoteExecutor alone(remote_options("remote-alone", first->key(), 1));
  InMemoryResultCommitSink one_sink;
  expect_succeeded(run(graph, {&alone}, one_sink), "one-worker run");
  alone.stop();

  RemoteExecutor remote_a(remote_options("remote-a", first->key(), 1));
  RemoteExecutor remote_b(remote_options("remote-b", second->key(), 1));
  InMemoryResultCommitSink two_sink;
  expect_succeeded(run(graph, {&remote_a, &remote_b}, two_sink), "two-worker run");

  expect_identical(graph, one_sink, two_sink, "one vs two workers");
  std::set<std::string> executors;
  for (const CommittedResult& committed : two_sink.results()) {
    executors.insert(committed.executor_id);
  }
  expect(executors == std::set<std::string>{"remote-a", "remote-b"}, "both workers ran tasks");
}

void test_killed_worker_is_retried_after_restart() {
  Workers workers;
  auto worker = workers.start("svp-remote-exec");
  const PairingKey key = worker->key();
  const TaskGraph graph = make_toy_graph({single(ToyFault::none, std::nullopt, kKillableTaskMs)});
  RemoteExecutor remote(remote_options("remote", key));
  InMemoryResultCommitSink sink;
  EventLog log;
  std::vector<Executor*> executors{&remote};
  auto outcome = std::async(std::launch::async, [&] {
    return run(graph, executors, sink, test_policy(), log.observer());
  });
  // Wait until the task runs on the worker: connected, and heartbeating.
  const auto deadline = std::chrono::steady_clock::now() + kTestWaitLimit;
  while (remote.connections_opened() == 0) {
    expect(std::chrono::steady_clock::now() < deadline, "never connected");
    std::this_thread::sleep_for(kTestMaxIdleWait);
  }
  std::this_thread::sleep_for(5 * kTestHeartbeatInterval);
  worker->kill();
  // launchd would restart the daemon after a while; do the same. It may get
  // another port, and the coordinator must find it again by pairing id.
  std::this_thread::sleep_for(kWorkerDowntime);
  worker = workers.start(key);

  const BuildOutcome result = outcome.get();
  expect_succeeded(result, "killed-worker run");
  expect(result.stats.retries == 1, "one retry");
  expect(sink.find("task.toy.single")->result.attempt == 2, "attempt 2 committed");
  expect(remote.connections_opened() == 2, "rediscovered and reconnected");
  expect(has_failure(log.events(), "executor_lost"), "kill reported as executor_lost");
}

void test_corrupted_payload_quarantines_and_retries_in_process() {
  Workers workers;
  const auto worker = workers.start("svp-remote-exec");
  const TaskGraph graph = make_toy_graph({single(ToyFault::corrupt_in_transit)});
  RemoteExecutor remote(remote_options("remote", worker->key()));
  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;
  const BuildOutcome outcome = run(graph, {&remote, &local}, sink);
  expect_succeeded(outcome, "corruption run");
  expect(outcome.stats.invalid_results == 1, "one invalid result");
  expect(outcome.stats.quarantined_executors == std::vector<std::string>{"remote"},
         "remote quarantined");
  const CommittedResult* committed = sink.find("task.toy.single");
  expect(committed->executor_id == "in-process", "retried in-process");
}

void test_hang_expires_lease_and_retries() {
  Workers workers;
  const auto first = workers.start("svp-remote-exec-a");
  const auto second = workers.start("svp-remote-exec-b");
  const std::filesystem::path marker = workers.directory.path / "hung";
  const TaskGraph graph = make_toy_graph({single(ToyFault::hang, marker)});
  RemoteExecutor remote_a(remote_options("remote-a", first->key(), 1));
  RemoteExecutor remote_b(remote_options("remote-b", second->key(), 1));
  InMemoryResultCommitSink sink;
  EventLog log;
  ManualClock clock;
  const CancellationToken cancellation;
  std::vector<Executor*> executors{&remote_a, &remote_b};
  auto outcome = std::async(std::launch::async, [&] {
    return Scheduler(test_policy(), clock).run(graph, executors, sink, cancellation, log.observer());
  });

  // Once the worker has stopped itself, move time past the lease. A heartbeat
  // already in flight may renew it, so repeat until the expiry is seen, but
  // only while attempt 1 is the only lease.
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
  expect(sink.find("task.toy.single")->result.attempt >= 2, "a later attempt committed");
}

void test_cancel_stops_promptly() {
  Workers workers;
  const auto worker = workers.start("svp-remote-exec");
  const TaskGraph graph = make_toy_graph({single(ToyFault::none, std::nullopt, kLongTaskMs)});
  RemoteExecutor remote(remote_options("remote", worker->key()));
  InMemoryResultCommitSink sink;
  EventLog log;
  const SteadyClock clock;
  CancellationToken cancellation;
  std::vector<Executor*> executors{&remote};
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

void test_wrong_secret_never_runs_tasks() {
  Workers workers;
  const auto worker = workers.start("svp-remote-exec");
  PairingKey impostor = worker->key();
  impostor.secret = remote_test::random_bytes(remote::kMinPairingSecretBytes);
  const TaskGraph graph = make_toy_graph({single(ToyFault::none)});
  RemoteExecutor remote(remote_options("remote", impostor));
  InMemoryResultCommitSink sink;
  EventLog log;
  const BuildOutcome outcome = run(graph, {&remote}, sink, test_policy(), log.observer());
  expect(outcome.status != BuildStatus::succeeded && sink.size() == 0,
         "nothing ran without the secret");
  expect(has_failure(log.events(), "executor_lost", "authentication_failed"),
         "failures name the authentication failure");
  expect(remote.connections_opened() == 0, "no connection opened");
}

void test_unadvertised_pairing_is_lost() {
  const TaskGraph graph = make_toy_graph({single(ToyFault::none)});
  RemoteExecutorOptions options =
      remote_options("remote", remote_test::random_pairing("svp-remote-none"));
  options.connector.routes.discovery_timeout = kShortDiscovery;
  options.reconnect_window = std::chrono::milliseconds{0};
  RemoteExecutor remote(options);
  InMemoryResultCommitSink sink;
  EventLog log;
  const BuildOutcome outcome = run(graph, {&remote}, sink, test_policy(), log.observer());
  expect(outcome.status != BuildStatus::succeeded, "no worker, no build");
  expect(has_failure(log.events(), "executor_lost", "worker_not_found"),
         "failures name the missing worker");
}

// A build closes a worker's idle session once that worker's kind of task is
// done (freeing what its session process holds); a later task of that kind
// (a retry) must still run, on a fresh session. A session with an
// outstanding lease is never closed.
void test_idle_session_closes_and_reopens() {
  Workers workers;
  const auto worker = workers.start("svp-remote-idle");
  const ToyTask first{.task_id = "task.toy.first",
                      .seed = 1,
                      .order_key = {.lane = "alpha", .ordinals = {0}}};
  const ToyTask second{.task_id = "task.toy.second",
                       .depends_on = {first.task_id},
                       .seed = 2,
                       .order_key = {.lane = "alpha", .ordinals = {1}}};
  const TaskGraph graph = make_toy_graph({first, second});
  RemoteExecutor remote(remote_options("remote", worker->key(), 1));
  bool closed_busy = true;
  bool closed_idle = false;
  const AttemptObserver observer = [&](const AttemptEvent& event) {
    if (event.task_id != first.task_id) return;
    if (event.kind == AttemptEventKind::leased) closed_busy = remote.close_idle_session();
    if (event.kind == AttemptEventKind::committed) closed_idle = remote.close_idle_session();
  };
  InMemoryResultCommitSink sink;
  expect_succeeded(run(graph, {&remote}, sink, test_policy(), observer),
                   "a task after the idle session closed still runs");
  expect(!closed_busy, "a session with an outstanding lease is not closed");
  expect(closed_idle, "an idle session is closed");
  expect(remote.connections_opened() == 2, "the next task opened a fresh session");
  expect_equal(to_text(sink.find(second.task_id)->payloads.front()),
               toy_expected_output(second.task_id, second.seed), "the fresh session's result");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: svp-exec-remote-executor-tests <svp-exec-test-worker>\n";
    return 2;
  }
  g_worker_path = argv[1];
  return run_tests(
      "svp-exec-remote-executor-tests",
      {
          {"normal run", test_normal_run},
          {"matches in-process and loopback", test_matches_in_process_and_loopback},
          {"two workers match one", test_two_workers_match_one},
          {"killed worker is retried after restart", test_killed_worker_is_retried_after_restart},
          {"corrupted payload quarantines and retries in-process",
           test_corrupted_payload_quarantines_and_retries_in_process},
          {"hang expires lease and retries", test_hang_expires_lease_and_retries},
          {"cancel stops promptly", test_cancel_stops_promptly},
          {"wrong secret never runs tasks", test_wrong_secret_never_runs_tasks},
          {"unadvertised pairing is lost", test_unadvertised_pairing_is_lost},
          {"idle session closes and reopens", test_idle_session_closes_and_reopens},
      });
}
