// The coordinator waiting for a worker whose service is about to move to a
// newer runtime (worker_restart_wait.hpp): predicting the switch from
// HELLO_ACK, and waiting until the worker answers on the new runtime, with
// a bounded deadline, without waiting when it will not switch.

#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "svp/exec/worker/launchd_job.hpp"
#include "svp/exec/worker/runtime_test_start.hpp"
#include "svp/exec/worker/worker_restart_wait.hpp"
#include "worker_test_support.hpp"

#include <deque>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
using svp::exec::Blake3Digest;
using svp::exec::RuntimeRelease;

const Blake3Digest kOld = svp::exec::blake3_digest(std::string_view("old"));
const Blake3Digest kNew = svp::exec::blake3_digest(std::string_view("new"));

WorkerHelloAck ack_on(const Blake3Digest& agent, std::optional<std::uint64_t> stamp,
                      bool self_update = true, std::uint64_t sessions = 1) {
  WorkerHelloAck ack;
  ack.agent_runtime_id = agent;
  ack.active_sessions = sessions;
  ack.service = ServiceUpdateState{
      .self_update = self_update, .release_stamp = stamp, .declined_runtimes = {}};
  return ack;
}

const RuntimeRelease kPushed{.runtime_id = kNew, .release_stamp = 20};

void test_predicts_the_switch() {
  expect(worker_will_switch_to(ack_on(kOld, 10), kPushed), "a newer stamped runtime switches it");
  expect(worker_will_switch_to(ack_on(kOld, std::nullopt), kPushed),
         "a service on an unstamped runtime moves to a stamped one");
  expect(!worker_will_switch_to(ack_on(kOld, 30), kPushed), "an older runtime does not");
  expect(!worker_will_switch_to(ack_on(kNew, 20), kPushed), "nor the runtime it already runs");
  expect(!worker_will_switch_to(ack_on(kOld, 10, false), kPushed),
         "nor a service that does not update itself");
  expect(!worker_will_switch_to(ack_on(kOld, 10),
                                RuntimeRelease{.runtime_id = kNew, .release_stamp = {}}),
         "nor an unstamped runtime");
  WorkerHelloAck declined = ack_on(kOld, 10);
  declined.service->declined_runtimes = {kNew};
  expect(!worker_will_switch_to(declined, kPushed), "nor a runtime it declined");
  WorkerHelloAck old_worker = ack_on(kOld, 10);
  old_worker.service.reset();
  expect(!worker_will_switch_to(old_worker, kPushed),
         "a worker that predates self-update never switches");
}

void test_deadline_is_bounded_and_scales() {
  const auto base = worker_restart_deadline(0);
  expect(base == std::chrono::milliseconds(kTestStartLaunchAllowance) +
                     std::chrono::seconds(kWorkerJobThrottleSeconds) +
                     svp::exec::remote::kDefaultAdvertiseTimeout +
                     svp::exec::remote::kDefaultDiscoveryTimeout,
         "an empty runtime: launch, throttle, advertise, discovery");
  expect(worker_restart_deadline(kTestStartMinVerifyBytesPerSecond) ==
             base + std::chrono::seconds(2),
         "every byte is verified twice (service, then test-start)");
}

// A scripted worker: each probe takes the next answer; time advances by
// each sleep.
struct Script {
  std::deque<WorkerProbeResult> answers;
  std::chrono::steady_clock::time_point clock{};
  std::uint64_t sleeps = 0;

  RestartWaitHooks hooks() {
    RestartWaitHooks hooks;
    hooks.probe = [this] {
      if (answers.empty()) {
        return WorkerProbeResult{.ack = {}, .error = "connection refused", .permanent = false};
      }
      WorkerProbeResult next = answers.front();
      answers.pop_front();
      return next;
    };
    hooks.now = [this] { return clock; };
    hooks.sleep = [this](std::chrono::milliseconds pause) {
      clock += pause;
      ++sleeps;
    };
    return hooks;
  }
};

WorkerProbeResult unreachable(std::string error = "connection refused") {
  return WorkerProbeResult{.ack = {}, .error = std::move(error), .permanent = false};
}

WorkerProbeResult answers(WorkerHelloAck ack) {
  return WorkerProbeResult{.ack = std::move(ack), .error = {}, .permanent = false};
}

void test_waits_until_the_worker_answers_on_the_new_runtime() {
  Script script;
  script.answers = {answers(ack_on(kOld, 10)), unreachable(), unreachable("not advertised"),
                    answers(ack_on(kNew, 20))};
  const RestartWaitOutcome outcome =
      wait_for_worker_restart(kPushed, std::chrono::seconds(30), script.hooks());
  expect(outcome.end == RestartWaitEnd::settled, "settled");
  expect(outcome.ack && outcome.ack->agent_runtime_id == kNew, "on the new runtime");
  expect(outcome.attempts == 4 && script.sleeps == 3, "probed until it came back");
}

void test_does_not_wait_when_the_switch_was_declined() {
  Script script;
  WorkerHelloAck declined = ack_on(kOld, 10);
  declined.service->declined_runtimes = {kNew};
  script.answers = {unreachable(), answers(declined)};
  const RestartWaitOutcome outcome =
      wait_for_worker_restart(kPushed, std::chrono::seconds(30), script.hooks());
  expect(outcome.end == RestartWaitEnd::settled && outcome.attempts == 2,
         "a failed test-start ends the wait as soon as the worker says so");
}

void test_other_sessions_defer_the_switch() {
  Script script;
  script.answers = {answers(ack_on(kOld, 10, true, 3))};
  const RestartWaitOutcome outcome =
      wait_for_worker_restart(kPushed, std::chrono::seconds(30), script.hooks());
  expect(outcome.end == RestartWaitEnd::deferred && outcome.attempts == 1,
         "with other coordinators' sessions live, waiting does not help");
}

void test_gives_up_at_the_deadline() {
  Script script;
  const RestartWaitOutcome outcome =
      wait_for_worker_restart(kPushed, std::chrono::seconds(5), script.hooks());
  expect(outcome.end == RestartWaitEnd::timed_out, "timed out");
  expect(script.clock - std::chrono::steady_clock::time_point{} <= std::chrono::seconds(5),
         "never past the deadline");
  expect(outcome.attempts == std::chrono::seconds(5) / kWorkerRestartPollPause,
         "one probe per poll pause");
  expect_equal(outcome.last_error, std::string("connection refused"), "the last failure");
}

void test_permanent_failures_end_the_wait() {
  Script script;
  script.answers = {WorkerProbeResult{.ack = {}, .error = "authentication", .permanent = true}};
  expect(wait_for_worker_restart(kPushed, std::chrono::seconds(30), script.hooks()).end ==
             RestartWaitEnd::refused,
         "an authentication failure does not wait");
  Script refusing;
  WorkerHelloAck refusal = ack_on(kNew, 20);
  refusal.refusal = SessionRefusal{.code = SessionRefusalCode::os_mismatch, .message = "macOS"};
  refusing.answers = {answers(refusal)};
  expect(wait_for_worker_restart(kPushed, std::chrono::seconds(30), refusing.hooks()).end ==
             RestartWaitEnd::refused,
         "a refused HELLO does not wait");
}

void test_cancellation_ends_the_wait() {
  Script script;
  RestartWaitHooks hooks = script.hooks();
  hooks.cancelled = [] { return true; };
  const RestartWaitOutcome outcome = wait_for_worker_restart(kPushed, std::chrono::seconds(30), hooks);
  expect(outcome.attempts == 1 && script.sleeps == 0, "a cancelled build stops waiting");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec-worker-restart-wait-tests",
      {
          {"predicts the switch", test_predicts_the_switch},
          {"deadline is bounded and scales", test_deadline_is_bounded_and_scales},
          {"waits until the worker answers on the new runtime",
           test_waits_until_the_worker_answers_on_the_new_runtime},
          {"does not wait when the switch was declined",
           test_does_not_wait_when_the_switch_was_declined},
          {"other sessions defer the switch", test_other_sessions_defer_the_switch},
          {"gives up at the deadline", test_gives_up_at_the_deadline},
          {"permanent failures end the wait", test_permanent_failures_end_the_wait},
          {"cancellation ends the wait", test_cancellation_ends_the_wait},
      });
}
