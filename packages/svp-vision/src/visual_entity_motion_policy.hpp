#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace svp::vision::visual_entity_internal {

struct GlobalMotionPolicy {
  // Three independently supported regions spanning the raster distinguish a
  // distributed effect from a dominant subject plus one background region.
  std::size_t minimum_cluster_count = 3;

  // When the enclosing motion extent covers 90% of the raster, motion boxes
  // represent a transition/global effect rather than localized entities.
  double minimum_enclosing_area_ratio = 0.90;
};

[[nodiscard]] bool has_global_motion_shape(
    const std::vector<cv::Rect>& cluster_boxes,
    int frame_width,
    int frame_height,
    const GlobalMotionPolicy& policy = {});

}  // namespace svp::vision::visual_entity_internal
