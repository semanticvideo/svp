#include "svp/media/canonical_raster.hpp"

#include <numeric>
#include <stdexcept>

namespace svp::media {
namespace {

bool swaps_axes(std::int32_t rotation_degrees) {
  const std::int32_t normalized = ((rotation_degrees % 360) + 360) % 360;
  return normalized == 90 || normalized == 270;
}

std::int32_t nearest_even_dimension(std::int64_t numerator, std::int64_t denominator) {
  if (numerator <= 0 || denominator <= 0) {
    throw std::invalid_argument("canonical raster input dimensions must be positive");
  }

  const std::int64_t floor_value = numerator / denominator;
  std::int64_t lower_even = floor_value - (floor_value % 2);
  if (lower_even < 2) {
    lower_even = 2;
  }
  const std::int64_t upper_even = lower_even + 2;

  const auto distance = [numerator, denominator](std::int64_t candidate) {
    const __int128 scaled_candidate =
        static_cast<__int128>(candidate) * static_cast<__int128>(denominator);
    const __int128 scaled_value = static_cast<__int128>(numerator);
    return scaled_candidate > scaled_value ? scaled_candidate - scaled_value
                                           : scaled_value - scaled_candidate;
  };

  const __int128 lower_distance = distance(lower_even);
  const __int128 upper_distance = distance(upper_even);

  return static_cast<std::int32_t>(upper_distance <= lower_distance ? upper_even
                                                                    : lower_even);
}

std::string ratio_string(std::int64_t width_units, std::int64_t height_units) {
  const std::int64_t divisor = std::gcd(width_units, height_units);
  return std::to_string(width_units / divisor) + ":" +
         std::to_string(height_units / divisor);
}

}  // namespace

DisplayGeometry compute_display_geometry(SourceDisplayGeometry source) {
  const Rational pixel_aspect_ratio = normalize(source.pixel_aspect_ratio);
  if (source.stored_width <= 0 || source.stored_height <= 0 ||
      pixel_aspect_ratio.numerator <= 0 || pixel_aspect_ratio.denominator <= 0) {
    throw std::invalid_argument("source display geometry must be positive");
  }

  const bool rotated = swaps_axes(source.rotation_degrees);
  const std::int32_t oriented_width = rotated ? source.stored_height : source.stored_width;
  const std::int32_t oriented_height = rotated ? source.stored_width : source.stored_height;
  const std::int64_t display_width_units =
      static_cast<std::int64_t>(oriented_width) * pixel_aspect_ratio.numerator;
  const std::int64_t display_height_units =
      static_cast<std::int64_t>(oriented_height) * pixel_aspect_ratio.denominator;

  return DisplayGeometry{
      oriented_width,
      oriented_height,
      display_width_units,
      display_height_units,
      ratio_string(display_width_units, display_height_units),
  };
}

CanonicalAnalysisRaster compute_canonical_analysis_raster(SourceDisplayGeometry source) {
  DisplayGeometry display = compute_display_geometry(source);

  std::int32_t width = kCanonicalLongestDisplayDimension;
  std::int32_t height = kCanonicalLongestDisplayDimension;

  if (display.display_width_units > display.display_height_units) {
    height = nearest_even_dimension(
        static_cast<std::int64_t>(kCanonicalLongestDisplayDimension) *
            display.display_height_units,
        display.display_width_units);
  } else if (display.display_height_units > display.display_width_units) {
    width = nearest_even_dimension(
        static_cast<std::int64_t>(kCanonicalLongestDisplayDimension) *
            display.display_width_units,
        display.display_height_units);
  }

  return CanonicalAnalysisRaster{width, height, display};
}

}  // namespace svp::media
