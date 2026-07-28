#pragma once

#include <cstdint>
#include <vector>

namespace svp::vision {

struct VisualEntitySamplingOptions {
  // Five observations per second bounds the unobserved interval while keeping
  // optical-flow displacement reasonable at the canonical analysis raster.
  std::int64_t sample_interval_us = 200000;

  // Decoding operates in bounded windows. Five seconds contains 26 samples
  // at the default cadence.
  std::int64_t window_duration_us = 5000000;

  // One second of shared evidence gives adjacent windows enough observations
  // to reconcile identities across decode windows.
  std::int64_t window_overlap_us = 1000000;
};

struct VisualEntitySamplingWindow {
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<std::int64_t> timestamps_us;
};

// Produces deterministic cadence-aligned timestamps through the supplied
// duration. Each decode window is independently bounded and adjacent windows
// overlap for identity handoff.
[[nodiscard]] std::vector<VisualEntitySamplingWindow>
make_visual_entity_sampling_plan(
    std::int64_t duration_us,
    const VisualEntitySamplingOptions& options = {});

}  // namespace svp::vision
