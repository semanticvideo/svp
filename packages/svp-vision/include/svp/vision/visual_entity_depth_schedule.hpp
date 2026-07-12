#pragma once

#include "svp/vision/color_frame_sampling.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace svp::vision {

struct VisualEntityDepthScheduleOptions {
  std::int64_t periodic_interval_us = 1000000;

  // A normalized mean RGB difference of 0.08 represents a substantial edit
  // rather than ordinary subject motion or sensor noise.
  double scene_change_threshold = 0.08;

  // Three consecutive depth observations establish minimum persistence for
  // a static entity introduced by an edit.
  std::size_t scene_change_burst_frames = 3;
};

[[nodiscard]] std::vector<std::size_t> select_visual_entity_depth_frames(
    const std::vector<ColorRasterFrame>& frames,
    const VisualEntityDepthScheduleOptions& options = {});

}  // namespace svp::vision
