#pragma once

// The worker service moving itself to a newer runtime (over the air): no
// password, no sudo, no script, at any fleet size.
//
// Coordinators push their runtime to every worker they use (RUNTIME_PUT,
// verified by BLAKE3 before it is installed). Task sessions already run from
// the coordinator's runtime; only the long-running service stays on the
// runtime <root>/current names (service_link.hpp). This class moves it:
//
//   1. Candidate: the newest installed runtime under the release order of
//      runtime_release.hpp that is newer than the service's own. Never an
//      older or unstamped one (no downgrade), never one whose test-start
//      already failed in this process.
//   2. Only while no session is live. Sessions enter through
//      try_enter_session(); while a candidate is being tested and after the
//      switch the gate is closed, so no session starts on a service that is
//      about to exit (its coordinator reconnects within the reconnect window
//      sized from launchd's ThrottleInterval, launchd_job.hpp).
//   3. Verify the candidate here (every file against its manifest), then
//      test-start it (runtime_test_start.hpp). On failure: stay on the
//      current runtime, log why, reopen the gate, and try the next candidate
//      if there is one.
//   4. Point <root>/current at it, then request_restart(): the agent stops
//      and exits with kWorkerRestartExitCode, and launchd's KeepAlive
//      {SuccessfulExit: false} starts <root>/current/bin/svp-builder again,
//      now the new runtime.
//
// Checked when the agent starts, after every runtime a session installs, and
// whenever a session ends. Disabled (with a logged reason) when the service
// was not started through <root>/current, or its own runtime is unknown.
// Old runtimes are never deleted here.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/runtime_release.hpp"
#include "svp/exec/worker/runtime_test_start.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <sysexits.h>

namespace svp::exec::worker {

// Exit status of a service that moved itself to a newer runtime. Nonzero, so
// KeepAlive {SuccessfulExit: false} restarts it; EX_TEMPFAIL (sysexits.h:
// "temporary failure, try again later") because a restart is exactly what
// it asks of launchd. No other svp-builder path exits with it.
inline constexpr int kWorkerRestartExitCode = EX_TEMPFAIL;

using RuntimeTestStarter = std::function<TestStartOutcome(
    const std::filesystem::path& runtime_dir, const Blake3Digest& runtime_id,
    std::uint64_t runtime_bytes)>;

struct ServiceUpdaterOptions {
  WorkerLayout layout;
  // The runtime this service runs from; nullopt disables updating.
  std::optional<Blake3Digest> own_runtime;
  // Whether the service was started through <root>/current
  // (launched_through_current); false disables updating.
  bool launched_through_current = false;
  // Default: test_start_runtime with test_start_deadline(runtime_bytes).
  RuntimeTestStarter test_start;
  // One line per decision; default discards.
  std::function<void(const std::string&)> log;
  // Called once, after `current` names the new runtime.
  std::function<void()> request_restart;
};

enum class UpdateStep {
  // Updating is off for this service (disabled_reason()).
  disabled,
  // No runtime newer than the service's own is installed.
  none,
  // A newer runtime is installed; sessions are live.
  waiting_for_idle,
  // Another thread is testing a candidate, or the switch is done.
  busy,
  // Every newer candidate failed its test-start; the service stays.
  failed,
  // `current` names the new runtime and a restart was requested.
  switched,
};

[[nodiscard]] std::string_view update_step_name(UpdateStep step) noexcept;

class ServiceUpdater {
 public:
  explicit ServiceUpdater(ServiceUpdaterOptions options);

  [[nodiscard]] bool enabled() const noexcept { return disabled_reason_.empty(); }
  [[nodiscard]] const std::string& disabled_reason() const noexcept { return disabled_reason_; }

  // False while a candidate is being tested or after the switch; the caller
  // then closes the connection without serving it.
  [[nodiscard]] bool try_enter_session();
  // Ends a session entered with try_enter_session(), then consider().
  void leave_session();
  // A session installed a runtime: consider().
  void runtime_installed();

  // Steps 1-4 above, synchronously.
  UpdateStep consider();

  [[nodiscard]] bool restart_requested() const;
  [[nodiscard]] std::uint64_t live_sessions() const;

 private:
  struct Candidate {
    RuntimeRelease release;
    std::uint64_t bytes = 0;
  };
  // Caller holds mutex_.
  [[nodiscard]] std::optional<Candidate> newest_candidate() const;
  void log(const std::string& line) const;

  ServiceUpdaterOptions options_;
  std::string disabled_reason_;
  RuntimeRelease own_{};
  mutable std::mutex mutex_;
  std::uint64_t live_ = 0;
  bool testing_ = false;
  bool restarting_ = false;
  std::set<Blake3Digest> failed_;
  std::optional<Blake3Digest> waiting_logged_;
};

}  // namespace svp::exec::worker
