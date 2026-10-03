#include "stream_deadline.hpp"

namespace svp::builder::workers {

StreamDeadline::StreamDeadline(svp::exec::remote::RemoteStream& stream,
                               std::chrono::milliseconds limit)
    : watcher_([this, &stream, limit] {
        {
          std::unique_lock lock(mutex_);
          if (stopped_.wait_for(lock, limit, [this] { return stop_; })) {
            return;
          }
          expired_ = true;
        }
        stream.cancel();
      }) {}

StreamDeadline::~StreamDeadline() {
  {
    const std::lock_guard lock(mutex_);
    stop_ = true;
  }
  stopped_.notify_all();
  watcher_.join();
}

bool StreamDeadline::expired() const {
  const std::lock_guard lock(mutex_);
  return expired_;
}

}  // namespace svp::builder::workers
