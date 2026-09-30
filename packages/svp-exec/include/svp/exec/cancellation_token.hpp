#pragma once

#include <atomic>

namespace svp::exec {

// Cooperative build cancellation (plan §4.4: Ctrl-C / SIGTERM). The scheduler
// polls it between dispatch rounds and never leases new work once it is set.
// request() is async-signal-safe (a lock-free atomic store), so a signal
// handler may call it.
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

}  // namespace svp::exec
