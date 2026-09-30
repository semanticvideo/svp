#include "svp/builder/progress_timeline.hpp"

#include <utility>

namespace svp::builder {
namespace {

std::int64_t milliseconds_between(TimestampedProgressSink::Clock::time_point from,
                                  TimestampedProgressSink::Clock::time_point to) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from)
      .count();
}

}  // namespace

TimestampedProgressSink::TimestampedProgressSink(
    std::vector<std::shared_ptr<BuildProgressSink>> downstream,
    Clock::time_point origin)
    : downstream_(std::move(downstream)), origin_(origin) {}

void TimestampedProgressSink::emit(const ProgressEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  ProgressEvent stamped = event;
  stamped.seq = ++last_seq_;
  stamped.t_ms = milliseconds_between(origin_, Clock::now());
  for (const auto& sink : downstream_) {
    if (sink) sink->emit(stamped);
  }
}

std::int64_t TimestampedProgressSink::elapsed_ms() const {
  return milliseconds_between(origin_, Clock::now());
}

std::uint64_t TimestampedProgressSink::emitted_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_seq_;
}

std::shared_ptr<TimestampedProgressSink> make_timestamped_progress_sink(
    std::vector<std::shared_ptr<BuildProgressSink>> downstream,
    TimestampedProgressSink::Clock::time_point origin) {
  return std::make_shared<TimestampedProgressSink>(std::move(downstream),
                                                   origin);
}

}  // namespace svp::builder
