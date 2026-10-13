#include "working_copy/working_copy_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace svp::builder::working_copy {
namespace {

// 4:2:0 chroma covers 2x2 pixels, so encoders need even frame sides.
constexpr int kChromaBlockSide = 2;

int nearest_even(double value) {
  const auto blocks = static_cast<int>(std::lround(value / kChromaBlockSide));
  return std::max(1, blocks) * kChromaBlockSide;
}

bool rotated_a_quarter_turn(std::int32_t rotation_degrees) {
  const std::int32_t normalized = ((rotation_degrees % 360) + 360) % 360;
  return normalized == 90 || normalized == 270;
}

}  // namespace

std::optional<WorkingCopyGeometry> plan_working_copy_geometry(
    const svp::media::VideoStreamProbe& video, int max_resolution) {
  if (video.width <= 0 || video.height <= 0 || max_resolution < kChromaBlockSide) {
    return std::nullopt;
  }
  const bool swaps = rotated_a_quarter_turn(video.rotation_degrees);
  const int display_width = swaps ? video.height : video.width;
  const int display_height = swaps ? video.width : video.height;
  const int short_side = std::min(display_width, display_height);
  const int long_side = std::max(display_width, display_height);
  if (short_side <= max_resolution) {
    return std::nullopt;
  }
  const int copy_short_side = max_resolution - (max_resolution % kChromaBlockSide);
  const int copy_long_side = nearest_even(
      long_side * (static_cast<double>(copy_short_side) / static_cast<double>(short_side)));
  const bool landscape = display_width >= display_height;
  WorkingCopyGeometry geometry{.display_width = landscape ? copy_long_side : copy_short_side,
                               .display_height = landscape ? copy_short_side : copy_long_side};
  geometry.stored_width = swaps ? geometry.display_height : geometry.display_width;
  geometry.stored_height = swaps ? geometry.display_width : geometry.display_height;
  return geometry;
}

}  // namespace svp::builder::working_copy
