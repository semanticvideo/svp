#include "equivalence/geometry_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace svp::validation::equivalence {
namespace {

double area(const Box& box) noexcept {
  return std::max(0.0, box[2] - box[0]) * std::max(0.0, box[3] - box[1]);
}

}  // namespace

double box_iou(const Box& left, const Box& right) noexcept {
  if (left == right) {
    return 1.0;
  }
  const Box intersection{
      std::max(left[0], right[0]),
      std::max(left[1], right[1]),
      std::min(left[2], right[2]),
      std::min(left[3], right[3]),
  };
  const double overlap = area(intersection);
  const double union_area = area(left) + area(right) - overlap;
  return union_area > 0.0 ? overlap / union_area : 0.0;
}

double scaled_point_distance(double left_x,
                             double left_y,
                             double right_x,
                             double right_y,
                             double raster_width,
                             double raster_height) noexcept {
  const double dx = (left_x - right_x) * raster_width;
  const double dy = (left_y - right_y) * raster_height;
  return std::hypot(dx, dy);
}

}  // namespace svp::validation::equivalence
