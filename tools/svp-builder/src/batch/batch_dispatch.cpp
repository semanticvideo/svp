#include "batch_dispatch.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <exception>
#include <thread>

namespace svp::builder::batch {
namespace {

class WorkQueue {
 public:
  explicit WorkQueue(std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      pending_.push_back(index);
    }
  }

  // The next item, or false once every item is done. Waits while the queue
  // is empty but items are still in flight (one may come back).
  bool take(std::size_t& index) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return !pending_.empty() || in_flight_ == 0; });
    if (pending_.empty()) {
      return false;
    }
    index = pending_.front();
    pending_.pop_front();
    ++in_flight_;
    return true;
  }

  void done() {
    {
      const std::lock_guard lock(mutex_);
      --in_flight_;
    }
    changed_.notify_all();
  }

  // Back to the front: it was next before, it is next again.
  void give_back(std::size_t index) {
    {
      const std::lock_guard lock(mutex_);
      pending_.push_front(index);
      --in_flight_;
    }
    changed_.notify_all();
  }

  // Waits up to `pause`, or until every item is done.
  void pause(std::chrono::milliseconds pause) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, pause, [&] { return pending_.empty() && in_flight_ == 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::size_t> pending_;
  std::size_t in_flight_ = 0;
};

}  // namespace

void dispatch_batch(const BatchDispatchOptions& options) {
  WorkQueue queue(options.item_count);
  std::vector<std::thread> threads;
  const std::size_t local_slots = options.local_slots == 0 ? 1 : options.local_slots;
  for (std::size_t slot = 0; slot < local_slots; ++slot) {
    threads.emplace_back([&] {
      std::size_t index = 0;
      while (queue.take(index)) {
        try {
          options.run_local(index);
        } catch (const std::exception& error) {
          if (options.on_local_error) options.on_local_error(index, error.what());
        } catch (...) {
          if (options.on_local_error) options.on_local_error(index, "unknown error");
        }
        queue.done();
      }
    });
  }
  for (const std::shared_ptr<RemoteVideoBuilder>& mac : options.remote_macs) {
    threads.emplace_back([&, mac] {
      std::size_t index = 0;
      while (queue.take(index)) {
        const RemoteVideoOutcome outcome = options.run_remote(index, *mac);
        switch (outcome.status) {
          case RemoteVideoStatus::built:
          case RemoteVideoStatus::failed:
            queue.done();
            break;
          case RemoteVideoStatus::busy:
            queue.give_back(index);
            queue.pause(options.busy_backoff);
            break;
          case RemoteVideoStatus::unavailable:
            queue.give_back(index);
            return;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

}  // namespace svp::builder::batch
