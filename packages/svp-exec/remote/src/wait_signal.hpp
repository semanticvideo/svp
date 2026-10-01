#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>

namespace svp::exec::remote::detail {

// One condition shared by a blocking connect and the framework callbacks it
// waits on. Waiters evaluate their predicate with `mutex` held; notifiers
// take `mutex` before notifying, so no change is lost between the two.
// Held by shared_ptr so a callback that fires late never touches freed
// memory.
struct WaitSignal {
  std::mutex mutex;
  std::condition_variable changed;
  bool cancelled = false;

  void notify() {
    { const std::lock_guard lock(mutex); }
    changed.notify_all();
  }
  void cancel() {
    {
      const std::lock_guard lock(mutex);
      cancelled = true;
    }
    changed.notify_all();
  }
};

}  // namespace svp::exec::remote::detail
