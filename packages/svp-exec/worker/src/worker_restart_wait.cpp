#include "svp/exec/worker/worker_restart_wait.hpp"

#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "svp/exec/worker/launchd_job.hpp"
#include "svp/exec/worker/runtime_test_start.hpp"

#include <algorithm>
#include <thread>

namespace svp::exec::worker {
namespace {

// The probing session itself is one of the worker's live sessions.
constexpr std::uint64_t kProbeSessions = 1;

}  // namespace

std::chrono::milliseconds worker_restart_deadline(std::uint64_t runtime_bytes) {
  const std::chrono::milliseconds service_verify(
      static_cast<std::int64_t>(runtime_bytes * 1000 / kTestStartMinVerifyBytesPerSecond));
  return service_verify + test_start_deadline(runtime_bytes) +
         std::chrono::seconds(kWorkerJobThrottleSeconds) + remote::kDefaultAdvertiseTimeout +
         remote::kDefaultDiscoveryTimeout;
}

bool worker_will_switch_to(const WorkerHelloAck& ack, const RuntimeRelease& runtime) {
  if (!ack.service || !ack.service->self_update || !ack.agent_runtime_id ||
      *ack.agent_runtime_id == runtime.runtime_id) {
    return false;
  }
  const auto& declined = ack.service->declined_runtimes;
  if (std::find(declined.begin(), declined.end(), runtime.runtime_id) != declined.end()) {
    return false;
  }
  return is_newer_release(runtime, RuntimeRelease{.runtime_id = *ack.agent_runtime_id,
                                                  .release_stamp = ack.service->release_stamp});
}

std::optional<RuntimeOffer> expected_switch(const WorkerHelloAck& ack,
                                            const std::optional<RuntimeOffer>& offered) {
  std::optional<RuntimeOffer> target;
  if (ack.service && ack.service->self_update && ack.service->pending &&
      ack.agent_runtime_id != ack.service->pending->runtime_id) {
    target = RuntimeOffer{.release = {.runtime_id = ack.service->pending->runtime_id,
                                      .release_stamp = ack.service->pending->release_stamp},
                          .bytes = ack.service->pending->bytes};
  }
  if (offered && worker_will_switch_to(ack, offered->release) &&
      (!target || is_newer_release(offered->release, target->release))) {
    target = offered;
  }
  return target;
}

std::string_view restart_wait_end_name(RestartWaitEnd end) noexcept {
  switch (end) {
    case RestartWaitEnd::settled:
      return "settled";
    case RestartWaitEnd::deferred:
      return "deferred";
    case RestartWaitEnd::timed_out:
      return "timed_out";
    case RestartWaitEnd::refused:
      return "refused";
  }
  return "unknown";
}

namespace {

RestartWaitOutcome wait_unguarded(const std::optional<RuntimeOffer>& offered,
                                           std::chrono::milliseconds deadline,
                                           const RestartWaitHooks& hooks) {
  const std::function<std::chrono::steady_clock::time_point()> now =
      hooks.now ? hooks.now : [] { return std::chrono::steady_clock::now(); };
  const std::function<void(std::chrono::milliseconds)> sleep =
      hooks.sleep ? hooks.sleep
                  : [](std::chrono::milliseconds pause) { std::this_thread::sleep_for(pause); };
  const auto give_up = now() + deadline;
  RestartWaitOutcome outcome;
  while (true) {
    ++outcome.attempts;
    const WorkerProbeResult result = hooks.probe();
    if (result.ack) {
      outcome.ack = result.ack;
      if (!result.ack->accepted()) {
        outcome.end = RestartWaitEnd::refused;
        outcome.last_error = result.ack->refusal->message;
        return outcome;
      }
      if (!expected_switch(*result.ack, offered)) {
        outcome.end = RestartWaitEnd::settled;
        return outcome;
      }
      if (result.ack->active_sessions > kProbeSessions) {
        outcome.end = RestartWaitEnd::deferred;
        return outcome;
      }
    } else {
      outcome.last_error = result.error;
      if (result.permanent) {
        outcome.end = RestartWaitEnd::refused;
        return outcome;
      }
    }
    if ((hooks.cancelled && hooks.cancelled()) || now() + kWorkerRestartPollPause >= give_up) {
      outcome.end = RestartWaitEnd::timed_out;
      return outcome;
    }
    sleep(kWorkerRestartPollPause);
  }
}

}  // namespace

RestartWaitOutcome wait_for_worker_restart(const std::optional<RuntimeOffer>& offered,
                                           std::chrono::milliseconds deadline,
                                           const RestartWaitHooks& hooks) {
  try {
    return wait_unguarded(offered, deadline, hooks);
  } catch (const std::exception& error) {
    // A clock or sleep that fails (std::system_error) ends the wait; the
    // next session finds the worker or reports it as before.
    return RestartWaitOutcome{.end = RestartWaitEnd::timed_out,
                              .ack = std::nullopt,
                              .attempts = 0,
                              .last_error = std::string("restart wait failed: ") + error.what()};
  }
}

void RuntimeSwitchWatch::observed(const WorkerHelloAck& ack) {
  const std::lock_guard lock(mutex_);
  if (expected_switch(ack, offered_)) {
    due_ = ack;
  } else {
    due_.reset();
  }
}

void RuntimeSwitchWatch::after_wait(const std::optional<WorkerHelloAck>& acknowledged,
                                    const WorkerHelloAck& due) {
  observed(acknowledged.value_or(due));
}

std::optional<WorkerHelloAck> RuntimeSwitchWatch::take_due() {
  const std::lock_guard lock(mutex_);
  std::optional<WorkerHelloAck> due = std::move(due_);
  due_.reset();
  return due;
}

}  // namespace svp::exec::worker
