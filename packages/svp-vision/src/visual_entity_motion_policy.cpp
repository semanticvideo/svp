#include "visual_entity_motion_policy.hpp"

#include <stdexcept>

namespace svp::vision::visual_entity_internal {

bool has_global_motion_shape(
    const std::vector<cv::Rect>& cluster_boxes,
    int frame_width,
    int frame_height,
    const GlobalMotionPolicy& policy) {
  if (frame_width <= 0 || frame_height <= 0) return false;
  if (policy.minimum_cluster_count < 2 ||
      policy.minimum_enclosing_area_ratio <= 0.0 ||
      policy.minimum_enclosing_area_ratio > 1.0) {
    throw std::invalid_argument("invalid global motion policy");
  }
  if (cluster_boxes.size() < policy.minimum_cluster_count) return false;

  cv::Rect enclosing;
  for (const auto& box : cluster_boxes) {
    if (box.empty()) continue;
    enclosing = enclosing.empty() ? box : enclosing | box;
  }
  enclosing &= cv::Rect(0, 0, frame_width, frame_height);
  const double frame_area = static_cast<double>(frame_width) * frame_height;
  return frame_area > 0.0 &&
      static_cast<double>(enclosing.area()) / frame_area >=
          policy.minimum_enclosing_area_ratio;
}

}  // namespace svp::vision::visual_entity_internal
