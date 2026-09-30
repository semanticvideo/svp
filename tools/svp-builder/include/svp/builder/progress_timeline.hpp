#pragma once

#include "svp/builder/build_progress.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace svp::builder {

// Stamps every progress event with `t_ms` (steady-clock milliseconds since
// `origin`) and `seq` (1, 2, 3, ... in emission order), then forwards it to
// each downstream sink. Stamping and forwarding happen under one lock, so
// downstream sinks observe events in `seq` order with non-decreasing `t_ms`
// even when the audio and vision lanes emit concurrently.
class TimestampedProgressSink final : public BuildProgressSink {
 public:
  using Clock = std::chrono::steady_clock;

  TimestampedProgressSink(
      std::vector<std::shared_ptr<BuildProgressSink>> downstream,
      Clock::time_point origin);

  void emit(const ProgressEvent& event) override;

  // Milliseconds since `origin` on the same clock used for `t_ms`.
  [[nodiscard]] std::int64_t elapsed_ms() const;
  // Number of events stamped so far (equals the last emitted `seq`).
  [[nodiscard]] std::uint64_t emitted_count() const;

 private:
  std::vector<std::shared_ptr<BuildProgressSink>> downstream_;
  Clock::time_point origin_;
  mutable std::mutex mutex_;
  std::uint64_t last_seq_ = 0;
};

std::shared_ptr<TimestampedProgressSink> make_timestamped_progress_sink(
    std::vector<std::shared_ptr<BuildProgressSink>> downstream,
    TimestampedProgressSink::Clock::time_point origin =
        TimestampedProgressSink::Clock::now());

}  // namespace svp::builder
