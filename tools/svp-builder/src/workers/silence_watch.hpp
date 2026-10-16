#pragma once

// Ends a coordinator's connection to a worker that has sent nothing for a
// lease period while it runs a task (the task's session heartbeats far more
// often than that), or, when given `stop`, once `stop` returns true (the
// build was cancelled).

#include "svp/exec/remote/remote_stream.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace svp::builder::workers {

// How often `stop` is asked: far inside the builder's interrupt grace
// period (build_interrupt.hpp), so a cancelled build lets go of its workers
// before the process is ended for it.
inline constexpr std::chrono::milliseconds kSilenceWatchStopPollInterval{100};

class SilenceWatch {
 public:
  SilenceWatch(svp::exec::remote::RemoteStream& stream, std::chrono::milliseconds limit,
               std::function<bool()> stop = {})
      : stream_(stream),
        limit_(limit),
        stop_(std::move(stop)),
        last_(std::chrono::steady_clock::now()) {
    thread_ = std::thread([this] { watch(); });
  }
  ~SilenceWatch() {
    {
      const std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }
  SilenceWatch(const SilenceWatch&) = delete;
  SilenceWatch& operator=(const SilenceWatch&) = delete;

  void heard() {
    const std::lock_guard lock(mutex_);
    last_ = std::chrono::steady_clock::now();
  }
  // The worker went silent for the limit.
  [[nodiscard]] bool expired() const {
    const std::lock_guard lock(mutex_);
    return expired_;
  }
  // `stop` ended the connection.
  [[nodiscard]] bool stopped_by_request() const {
    const std::lock_guard lock(mutex_);
    return stop_requested_;
  }

 private:
  void watch() {
    std::unique_lock lock(mutex_);
    while (!stopped_) {
      auto wake = last_ + limit_;
      if (stop_) {
        wake = std::min(wake, std::chrono::steady_clock::now() + kSilenceWatchStopPollInterval);
      }
      if (changed_.wait_until(lock, wake, [&] { return stopped_; })) {
        return;
      }
      if (stop_ && stop_()) {
        stop_requested_ = true;
        stream_.cancel();
        return;
      }
      if (std::chrono::steady_clock::now() >= last_ + limit_) {
        expired_ = true;
        stream_.cancel();
        return;
      }
    }
  }

  svp::exec::remote::RemoteStream& stream_;
  std::chrono::milliseconds limit_;
  std::function<bool()> stop_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::chrono::steady_clock::time_point last_;
  bool stopped_ = false;
  bool expired_ = false;
  bool stop_requested_ = false;
  std::thread thread_;
};

}  // namespace svp::builder::workers
