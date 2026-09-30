#pragma once

#include "svp/vision/visual_tracking_quality.hpp"

#include <cstdint>
#include <vector>

namespace svp::vision {

struct VisualEntitySamplingOptions {
  std::int64_t sample_interval_us;
  std::int64_t window_duration_us;
  std::int64_t window_overlap_us;
};

struct VisualEntitySamplingWindow {
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<std::int64_t> timestamps_us;
};

// Sampling cadence and windowing owned by a visual tracking quality policy.
[[nodiscard]] inline constexpr VisualEntitySamplingOptions
visual_entity_sampling_options(const VisualTrackingQualityPolicy& policy) noexcept {
  return {policy.sample_interval_us,
          policy.window_duration_us,
          policy.window_overlap_us};
}

// Produces deterministic cadence-aligned timestamps through the supplied
// duration. Each decode window is independently bounded and adjacent windows
// overlap for identity handoff.
[[nodiscard]] std::vector<VisualEntitySamplingWindow>
make_visual_entity_sampling_plan(
    std::int64_t duration_us,
    const VisualEntitySamplingOptions& options);

}  // namespace svp::vision
