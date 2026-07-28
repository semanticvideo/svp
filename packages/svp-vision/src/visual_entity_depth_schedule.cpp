#include "svp/vision/visual_entity_depth_schedule.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace svp::vision {
namespace {

double normalized_mean_difference(
    const ColorRasterFrame& left,
    const ColorRasterFrame& right) {
  if (left.width != right.width || left.height != right.height ||
      left.pixels.size() != right.pixels.size() || left.pixels.empty()) {
    return 1.0;
  }
  double difference_sum = 0.0;
  for (std::size_t index = 0; index < left.pixels.size(); ++index) {
    difference_sum += std::abs(
        static_cast<int>(left.pixels[index].r) - right.pixels[index].r);
    difference_sum += std::abs(
        static_cast<int>(left.pixels[index].g) - right.pixels[index].g);
    difference_sum += std::abs(
        static_cast<int>(left.pixels[index].b) - right.pixels[index].b);
  }
  return difference_sum /
      (static_cast<double>(left.pixels.size()) * 3.0 * 255.0);
}

}  // namespace

std::vector<std::size_t> select_visual_entity_depth_frames(
    const std::vector<ColorRasterFrame>& frames,
    const VisualEntityDepthScheduleOptions& options) {
  if (options.periodic_interval_us <= 0 ||
      options.scene_change_threshold <= 0.0 ||
      options.scene_change_threshold > 1.0 ||
      options.scene_change_burst_frames == 0) {
    throw std::invalid_argument("invalid visual entity depth schedule");
  }

  std::vector<std::size_t> selected;
  std::size_t burst_remaining = 0;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (index > 0 && normalized_mean_difference(
                         frames[index - 1], frames[index]) >=
                         options.scene_change_threshold) {
      burst_remaining = options.scene_change_burst_frames;
    }
    const bool periodic =
        frames[index].timestamp_us % options.periodic_interval_us == 0;
    if (periodic || burst_remaining > 0) selected.push_back(index);
    if (burst_remaining > 0) --burst_remaining;
  }
  return selected;
}

}  // namespace svp::vision
