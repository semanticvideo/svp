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
    const svp::media::VideoStreamProbe& video, int max_height) {
  if (video.width <= 0 || video.height <= 0 || max_height < kChromaBlockSide) {
    return std::nullopt;
  }
  const bool swaps = rotated_a_quarter_turn(video.rotation_degrees);
  const int display_width = swaps ? video.height : video.width;
  const int display_height = swaps ? video.width : video.height;
  if (display_height <= max_height) {
    return std::nullopt;
  }
  const int copy_display_height = max_height - (max_height % kChromaBlockSide);
  const double factor =
      static_cast<double>(copy_display_height) / static_cast<double>(display_height);
  const int copy_display_width = nearest_even(display_width * factor);
  WorkingCopyGeometry geometry{.display_width = copy_display_width,
                               .display_height = copy_display_height};
  geometry.stored_width = swaps ? copy_display_height : copy_display_width;
  geometry.stored_height = swaps ? copy_display_width : copy_display_height;
  return geometry;
}

}  // namespace svp::builder::working_copy
