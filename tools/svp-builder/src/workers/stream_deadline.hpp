#pragma once

// A time limit on one remote session: once it passes, the session's stream
// is cancelled, so a read blocked on a peer that never answers returns end
// of stream (RemoteStream::cancel) and the session fails through its own
// error path instead of waiting forever. Thread-safe.

#include "svp/exec/remote/remote_stream.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace svp::builder::workers {

class StreamDeadline {
 public:
  // Starts the clock. `stream` must outlive this object.
  StreamDeadline(svp::exec::remote::RemoteStream& stream, std::chrono::milliseconds limit);
  // Stops the clock; a stream not cancelled yet is left open.
  ~StreamDeadline();
  StreamDeadline(const StreamDeadline&) = delete;
  StreamDeadline& operator=(const StreamDeadline&) = delete;

  // True once the limit passed and the stream was cancelled.
  [[nodiscard]] bool expired() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable stopped_;
  bool stop_ = false;
  bool expired_ = false;
  std::thread watcher_;
};

}  // namespace svp::builder::workers
