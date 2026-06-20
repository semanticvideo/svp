#pragma once

#include "svp/media/rational.hpp"

#include <cstdint>
#include <string>

namespace svp::media {

inline constexpr std::int32_t kCanonicalLongestDisplayDimension = 640;

struct SourceDisplayGeometry {
  std::int32_t stored_width = 0;
  std::int32_t stored_height = 0;
  std::int32_t rotation_degrees = 0;
  Rational pixel_aspect_ratio{1, 1};
};

struct DisplayGeometry {
  std::int32_t oriented_width = 0;
  std::int32_t oriented_height = 0;
  std::int64_t display_width_units = 0;
  std::int64_t display_height_units = 0;
  std::string display_aspect_ratio;
};

struct CanonicalAnalysisRaster {
  std::int32_t width = 0;
  std::int32_t height = 0;
  DisplayGeometry display;
};

[[nodiscard]] DisplayGeometry compute_display_geometry(SourceDisplayGeometry source);
[[nodiscard]] CanonicalAnalysisRaster compute_canonical_analysis_raster(
    SourceDisplayGeometry source);

}  // namespace svp::media
