#pragma once

#include <array>

namespace svp::validation::equivalence {

// Axis-aligned box as [x_min, y_min, x_max, y_max] (RC2 Section 13 bbox
// convention for bbox_norm, bbox_px, box_norm, and box_px).
using Box = std::array<double, 4>;

// Intersection over union. Two identical degenerate (zero-area) boxes have
// IoU 1; otherwise a zero union yields 0.
[[nodiscard]] double box_iou(const Box& left, const Box& right) noexcept;

// Euclidean distance between two normalized points after scaling x by
// `raster_width` and y by `raster_height` pixels.
[[nodiscard]] double scaled_point_distance(double left_x,
                                           double left_y,
                                           double right_x,
                                           double right_y,
                                           double raster_width,
                                           double raster_height) noexcept;

}  // namespace svp::validation::equivalence
