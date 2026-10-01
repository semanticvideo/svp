#pragma once

// Ctrl-C and SIGTERM for builds (plan §4.4 cancellation, §7.3: "cancel all
// leases, stop sessions, keep journal").
//
// install_build_interrupt_handling() runs once in main, before any other
// thread starts. It blocks SIGINT and SIGTERM in every thread and handles them
// on one dedicated thread:
//   * While builds are registered (BuildInterruptScope), the first signal
//     cancels every registered build: each stops leasing tasks, records its
//     journal as interrupted, and returns BuildPipelineFailure::cancelled with
//     exit status 128 + signal. Stage code does not observe cancellation
//     inside a stage, so if the process is still running kInterruptGracePeriod
//     later, or a second signal arrives, it exits at once with the same status
//     after removing default staging directories. Either way the recovery
//     journal stays resumable: its commits are atomic and --resume discards
//     whatever an unfinished task left behind.
//   * With no build registered, a signal removes default staging directories
//     and exits with 128 + signal, as before.

#include <chrono>

namespace svp::exec {
class CancellationToken;
}

namespace svp::builder {

// Long enough for a commit in flight and for the scheduler to notice the
// cancellation between tasks (it polls every 50 ms); short enough that
// Ctrl-C still feels immediate while a long stage keeps running. Correctness
// does not depend on it (see above).
inline constexpr std::chrono::seconds kInterruptGracePeriod{5};

void install_build_interrupt_handling();

// Registers `cancellation` for the lifetime of the scope. Thread-safe; any
// number of builds may be registered at once (batch create).
class BuildInterruptScope {
 public:
  explicit BuildInterruptScope(svp::exec::CancellationToken& cancellation);
  ~BuildInterruptScope();
  BuildInterruptScope(const BuildInterruptScope&) = delete;
  BuildInterruptScope& operator=(const BuildInterruptScope&) = delete;

 private:
  svp::exec::CancellationToken& cancellation_;
};

// The signal that cancelled registered builds; 0 when none did.
[[nodiscard]] int build_interrupt_signal() noexcept;

}  // namespace svp::builder
