#pragma once

#include <atomic>
#include <string_view>

namespace svp::exec {

// Cooperative cancellation (plan §4.4). One token cancels a whole build (Ctrl-C
// / SIGTERM: the scheduler polls it between dispatch rounds and never leases
// new work once it is set); executors hand every attempt its own token and set
// it when the scheduler cancels that lease, including when the attempt runs
// past its hard deadline. request() is async-signal-safe (a lock-free atomic
// store), so a signal handler may call it.
class CancellationToken {
 public:
  void request() noexcept { requested_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool requested() const noexcept {
    return requested_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> requested_{false};
  static_assert(std::atomic<bool>::is_always_lock_free);
};

// For task functions: call at safe points (between frames, chunks, windows;
// plan §4.4). Throws ExecError(cancelled) naming `where` once the attempt's
// token is set. The result of a cancelled attempt is never committed, so a
// task function may stop at any such point without cleaning up its outputs.
void throw_if_cancelled(const CancellationToken& cancellation, std::string_view where);

}  // namespace svp::exec
