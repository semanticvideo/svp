#pragma once

// The coordinator's side of worker self-update (service_updater.hpp): a
// runtime newer than the one the worker's service runs, installed there by
// this coordinator or any other, makes the service switch to it and restart
// once its sessions end. A coordinator that opened its next session right
// then would find the worker gone (connection refused, or not advertised for
// a few seconds) and drop it from the command, build, or batch at hand. So
// when a session's HELLO_ACK shows a switch is due (the worker's reported
// pending switch, or the runtime this coordinator just installed there), the
// coordinator waits after that session, within worker_restart_deadline,
// until the worker answers HELLO with no switch due (on the new runtime, or
// having declined it), and only then opens its next session.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/runtime_release.hpp"
#include "svp/exec/worker/hello_messages.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace svp::exec::worker {

// Pause between reconnect attempts while the worker restarts: one discovery
// settle period (route_policy.hpp), long enough for mDNS caches to see the
// old service's record withdrawn and the new one announced, as
// kDefaultReconnectPause (remote_executor.hpp).
inline constexpr std::chrono::milliseconds kWorkerRestartPollPause{500};

// How long a worker's service may take from the end of the session that made
// it switch until it answers on the new runtime, summed from what it does:
//   - verify the runtime in the service, reading every file at
//     kTestStartMinVerifyBytesPerSecond (runtime_test_start.hpp);
//   - test-start it: test_start_deadline(runtime_bytes);
//   - wait out launchd's ThrottleInterval (kWorkerJobThrottleSeconds,
//     launchd_job.hpp) when the old service ran less than that;
//   - register the new service with Bonjour (kDefaultAdvertiseTimeout,
//     remote_listener.hpp);
//   - be found by one discovery round (kDefaultDiscoveryTimeout,
//     route_policy.hpp).
// Every term is a bound, not a typical time; a worker usually answers within
// a few seconds.
[[nodiscard]] std::chrono::milliseconds worker_restart_deadline(std::uint64_t runtime_bytes);

// A runtime and its total file bytes (which size the wait).
struct RuntimeOffer {
  RuntimeRelease release;
  std::uint64_t bytes = 0;

  bool operator==(const RuntimeOffer&) const = default;
};

// Whether the worker that answered `ack` will move its service to `runtime`
// (installed there, now or earlier): its service updates itself, does not
// run `runtime` already, has not declined it, and `runtime` is newer under
// the release order (runtime_release.hpp). False for a worker that does not
// report its service (it predates self-update and never switches).
[[nodiscard]] bool worker_will_switch_to(const WorkerHelloAck& ack, const RuntimeRelease& runtime);

// The runtime the worker that answered `ack` will restart on once its
// sessions end, when any: the pending switch it reports (hello_messages.hpp,
// whoever installed that runtime), and `offered` (a runtime this coordinator
// has installed there) when worker_will_switch_to it; the newer of the two
// under the release order. nullopt: no switch is due.
[[nodiscard]] std::optional<RuntimeOffer> expected_switch(const WorkerHelloAck& ack,
                                                          const std::optional<RuntimeOffer>& offered);

enum class RestartWaitEnd {
  // The worker answered on the runtime, or showed it will not switch to it
  // (declined it, or another runtime won): go on.
  settled,
  // Other sessions are live on the worker, so its switch waits for them;
  // waiting here does not help. Go on; the switch comes later.
  deferred,
  // The deadline passed; the worker may still be restarting.
  timed_out,
  // The worker refused HELLO or does not hold the pairing's secret.
  refused,
};

[[nodiscard]] std::string_view restart_wait_end_name(RestartWaitEnd end) noexcept;

struct RestartWaitOutcome {
  RestartWaitEnd end = RestartWaitEnd::timed_out;
  // The last HELLO_ACK the worker sent, when any.
  std::optional<WorkerHelloAck> ack;
  std::uint64_t attempts = 0;
  // The last failure to reach the worker, for reports.
  std::string last_error;
};

struct WorkerProbeResult {
  std::optional<WorkerHelloAck> ack;
  // Set when the worker could not be reached or answered no HELLO_ACK.
  std::string error;
  // The failure will not go away by waiting (authentication, refusal).
  bool permanent = false;
};

struct RestartWaitHooks {
  // One session: connect, HELLO, SHUTDOWN. Never throws.
  std::function<WorkerProbeResult()> probe;
  // Injectable for tests; defaults: steady_clock and this_thread::sleep_for.
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
  // Stops the wait early (a cancelled build); default never.
  std::function<bool()> cancelled;
};

// Probes until an ACK shows no switch due (expected_switch with `offered` is
// nullopt: settled) or that other sessions hold the switch (deferred), or
// `deadline` passes. Never throws: a hook that throws (a clock or sleep
// failing) ends the wait as timed_out with the reason in last_error.
[[nodiscard]] RestartWaitOutcome wait_for_worker_restart(const std::optional<RuntimeOffer>& offered,
                                                         std::chrono::milliseconds deadline,
                                                         const RestartWaitHooks& hooks);

// For a coordinator that opens one session after another to the same worker
// (a batch's whole-video jobs, paired_video_builder.hpp; a build's task
// sessions, worker_supplies.hpp): remembers that a session's HELLO_ACK
// showed a switch is due (expected_switch with `offered`, this
// coordinator's runtime, installed by the session), so the next session
// first waits for the restart instead of finding the worker gone.
// Thread-safe.
class RuntimeSwitchWatch {
 public:
  explicit RuntimeSwitchWatch(std::optional<RuntimeOffer> offered)
      : offered_(std::move(offered)) {}

  [[nodiscard]] const std::optional<RuntimeOffer>& offered() const noexcept { return offered_; }

  // A session was answered with `ack` (a later ack replaces an earlier one).
  void observed(const WorkerHelloAck& ack);

  // Before the next session: the ACK that showed the switch, when one is
  // due, and forgets it (the caller waits once, with
  // wait_for_worker_restart); nullopt when no switch is due.
  [[nodiscard]] std::optional<WorkerHelloAck> take_due();

  // After waiting for a due switch: the ACK the wait ended with, or the due
  // one when it ended with none (timed out, cancelled). A switch still due
  // (deferred, timed out) stays due, so the next session waits again.
  void after_wait(const std::optional<WorkerHelloAck>& acknowledged, const WorkerHelloAck& due);

 private:
  std::optional<RuntimeOffer> offered_;
  std::mutex mutex_;
  std::optional<WorkerHelloAck> due_;
};

}  // namespace svp::exec::worker
