#include "svp/vision/visual_entity_sampling.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::vision {
namespace {

void validate_options(const VisualEntitySamplingOptions& options) {
  if (options.sample_interval_us <= 0) {
    throw std::invalid_argument(
        "visual entity sample interval must be positive");
  }
  if (options.window_duration_us < options.sample_interval_us) {
    throw std::invalid_argument(
        "visual entity window must contain at least one sample interval");
  }
  if (options.window_overlap_us < options.sample_interval_us ||
      options.window_overlap_us >= options.window_duration_us) {
    throw std::invalid_argument(
        "visual entity window overlap must include at least one sample "
        "interval and remain shorter than the window");
  }
}

std::vector<std::int64_t> timestamps_for_window(
    std::int64_t start_us,
    std::int64_t end_us,
    std::int64_t interval_us) {
  std::vector<std::int64_t> timestamps;
  for (std::int64_t timestamp_us = start_us; timestamp_us <= end_us;) {
    timestamps.push_back(timestamp_us);
    if (end_us - timestamp_us < interval_us) break;
    timestamp_us += interval_us;
  }
  return timestamps;
}

}  // namespace

std::vector<VisualEntitySamplingWindow> make_visual_entity_sampling_plan(
    std::int64_t duration_us,
    const VisualEntitySamplingOptions& options) {
  validate_options(options);
  if (duration_us <= 0) return {};

  std::vector<VisualEntitySamplingWindow> windows;
  const std::int64_t advance_us =
      options.window_duration_us - options.window_overlap_us;

  for (std::int64_t start_us = 0; start_us < duration_us;) {
    const std::int64_t end_us =
        std::min(duration_us, start_us + options.window_duration_us);
    windows.push_back({
        start_us,
        end_us,
        timestamps_for_window(start_us, end_us, options.sample_interval_us)});
    if (end_us == duration_us) break;
    start_us += advance_us;
  }

  return windows;
}

}  // namespace svp::vision
