#include "equivalence/mask_metrics.hpp"

#include "equivalence/equivalence_profile.hpp"
#include "equivalence/payload_metrics.hpp"

#include "svp/blocks/block_stream.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace svp::validation::equivalence {
namespace {

constexpr unsigned kLeb128PayloadBits = 7;
constexpr std::uint8_t kLeb128PayloadMask = 0x7F;
constexpr std::uint8_t kLeb128ContinueBit = 0x80;
constexpr unsigned kLeb128MaxShift = 63;
constexpr unsigned kBitsPerByte = 8;

std::optional<std::vector<std::uint8_t>> decode_rle(std::span<const std::byte> payload,
                                                    std::size_t pixel_count) {
  std::vector<std::uint8_t> pixels;
  pixels.reserve(pixel_count);
  std::uint8_t current = 0;  // runs start with background
  std::size_t position = 0;
  while (position < payload.size()) {
    std::uint64_t run = 0;
    unsigned shift = 0;
    while (true) {
      if (position >= payload.size() || shift > kLeb128MaxShift) {
        return std::nullopt;
      }
      const auto byte = std::to_integer<std::uint8_t>(payload[position++]);
      run |= static_cast<std::uint64_t>(byte & kLeb128PayloadMask) << shift;
      if ((byte & kLeb128ContinueBit) == 0) {
        break;
      }
      shift += kLeb128PayloadBits;
    }
    if (run > pixel_count - pixels.size()) {
      return std::nullopt;
    }
    pixels.insert(pixels.end(), static_cast<std::size_t>(run), current);
    current = current == 0 ? 1 : 0;
  }
  if (pixels.size() != pixel_count) {
    return std::nullopt;
  }
  return pixels;
}

std::optional<std::vector<std::uint8_t>> decode_bitpacked(std::span<const std::byte> payload,
                                                          std::size_t pixel_count) {
  if (payload.size() * kBitsPerByte < pixel_count) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> pixels(pixel_count);
  for (std::size_t index = 0; index < pixel_count; ++index) {
    const auto byte = std::to_integer<std::uint8_t>(payload[index / kBitsPerByte]);
    pixels[index] = static_cast<std::uint8_t>((byte >> (index % kBitsPerByte)) & 1U);
  }
  return pixels;
}

bool is_boundary(const MaskPlane& plane, std::uint32_t x, std::uint32_t y) {
  const auto at = [&](std::uint32_t px, std::uint32_t py) {
    return plane.pixels[static_cast<std::size_t>(py) * plane.width + px] != 0;
  };
  if (!at(x, y)) {
    return false;
  }
  return x == 0 || y == 0 || x + 1 == plane.width || y + 1 == plane.height ||
         !at(x - 1, y) || !at(x + 1, y) || !at(x, y - 1) || !at(x, y + 1);
}

std::vector<std::uint8_t> boundary_of(const MaskPlane& plane) {
  std::vector<std::uint8_t> boundary(plane.pixels.size(), 0);
  for (std::uint32_t y = 0; y < plane.height; ++y) {
    for (std::uint32_t x = 0; x < plane.width; ++x) {
      boundary[static_cast<std::size_t>(y) * plane.width + x] =
          is_boundary(plane, x, y) ? 1 : 0;
    }
  }
  return boundary;
}

// Exact 1-D squared Euclidean distance transform (Felzenszwalb and
// Huttenlocher, "Distance Transforms of Sampled Functions"). `values` must be
// finite; pixels without a seed carry a value larger than any reachable
// squared distance.
void distance_transform_1d(std::vector<double>& values) {
  const auto count = values.size();
  if (count == 0) {
    return;
  }
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  const auto parabola_intersection = [&](std::size_t q, std::size_t p) {
    const auto qd = static_cast<double>(q);
    const auto pd = static_cast<double>(p);
    return ((values[q] + qd * qd) - (values[p] + pd * pd)) / (2.0 * qd - 2.0 * pd);
  };

  std::vector<std::size_t> hull(count);
  std::vector<double> bounds(count + 1);
  std::size_t k = 0;
  hull[0] = 0;
  bounds[0] = -kInfinity;
  bounds[1] = kInfinity;
  for (std::size_t q = 1; q < count; ++q) {
    double s = parabola_intersection(q, hull[k]);
    while (s <= bounds[k]) {
      --k;
      s = parabola_intersection(q, hull[k]);
    }
    ++k;
    hull[k] = q;
    bounds[k] = s;
    bounds[k + 1] = kInfinity;
  }

  std::vector<double> output(count);
  k = 0;
  for (std::size_t q = 0; q < count; ++q) {
    while (bounds[k + 1] < static_cast<double>(q)) {
      ++k;
    }
    const double delta = static_cast<double>(q) - static_cast<double>(hull[k]);
    output[q] = delta * delta + values[hull[k]];
  }
  values = std::move(output);
}

// Squared distance from every pixel to the nearest set pixel of `seeds`.
std::vector<double> squared_distance_to(const std::vector<std::uint8_t>& seeds,
                                        std::uint32_t width,
                                        std::uint32_t height) {
  // Exceeds the squared diagonal of the raster, so it is never chosen over
  // a real seed and stays finite for the transform's arithmetic.
  const double far = 2.0 * (static_cast<double>(width) * width +
                            static_cast<double>(height) * height) + 1.0;
  std::vector<double> field(seeds.size());
  for (std::size_t index = 0; index < seeds.size(); ++index) {
    field[index] = seeds[index] != 0 ? 0.0 : far;
  }
  std::vector<double> line;
  for (std::uint32_t x = 0; x < width; ++x) {
    line.assign(height, 0.0);
    for (std::uint32_t y = 0; y < height; ++y) {
      line[y] = field[static_cast<std::size_t>(y) * width + x];
    }
    distance_transform_1d(line);
    for (std::uint32_t y = 0; y < height; ++y) {
      field[static_cast<std::size_t>(y) * width + x] = line[y];
    }
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto row = field.begin() + static_cast<std::ptrdiff_t>(y) * width;
    line.assign(row, row + width);
    distance_transform_1d(line);
    std::copy(line.begin(), line.end(), row);
  }
  return field;
}

void append_boundary_distances(const std::vector<std::uint8_t>& from,
                               const std::vector<double>& squared_to,
                               std::vector<double>& distances) {
  for (std::size_t index = 0; index < from.size(); ++index) {
    if (from[index] != 0) {
      distances.push_back(std::sqrt(squared_to[index]));
    }
  }
}

}  // namespace

bool MaskPlane::empty() const noexcept {
  return std::none_of(pixels.begin(), pixels.end(), [](std::uint8_t pixel) { return pixel != 0; });
}

std::optional<std::vector<MaskPlane>> decode_mask_planes(std::span<const std::byte> payload,
                                                         std::uint32_t dtype,
                                                         std::uint32_t width,
                                                         std::uint32_t height,
                                                         std::uint32_t plane_count) {
  const auto plane_pixels = static_cast<std::size_t>(width) * height;
  const auto pixel_count = plane_pixels * plane_count;
  std::optional<std::vector<std::uint8_t>> pixels;
  if (dtype == static_cast<std::uint32_t>(svp::blocks::DType::svp_rle_v1)) {
    pixels = decode_rle(payload, pixel_count);
  } else if (dtype == static_cast<std::uint32_t>(svp::blocks::DType::bitpacked_lsb_first)) {
    pixels = decode_bitpacked(payload, pixel_count);
  }
  if (!pixels.has_value()) {
    return std::nullopt;
  }

  std::vector<MaskPlane> planes(plane_count);
  for (std::uint32_t plane = 0; plane < plane_count; ++plane) {
    planes[plane].width = width;
    planes[plane].height = height;
    const auto begin = pixels->begin() + static_cast<std::ptrdiff_t>(plane * plane_pixels);
    planes[plane].pixels.assign(begin, begin + static_cast<std::ptrdiff_t>(plane_pixels));
  }
  return planes;
}

MaskMetrics mask_metrics(const MaskPlane& left, const MaskPlane& right) {
  MaskMetrics metrics;
  std::size_t intersection = 0;
  std::size_t union_count = 0;
  for (std::size_t index = 0; index < left.pixels.size(); ++index) {
    const bool a = left.pixels[index] != 0;
    const bool b = right.pixels[index] != 0;
    intersection += (a && b) ? 1 : 0;
    union_count += (a || b) ? 1 : 0;
  }
  metrics.iou = union_count == 0 ? 1.0
                                 : static_cast<double>(intersection) /
                                       static_cast<double>(union_count);

  const auto left_boundary = boundary_of(left);
  const auto right_boundary = boundary_of(right);
  std::vector<double> distances;
  append_boundary_distances(left_boundary,
                            squared_distance_to(right_boundary, right.width, right.height),
                            distances);
  append_boundary_distances(right_boundary,
                            squared_distance_to(left_boundary, left.width, left.height),
                            distances);
  metrics.boundary_displacement_px = nearest_rank_quantile(
      distances, DefaultEquivalenceProfileV1::kMaskBoundaryDisplacementQuantile);
  return metrics;
}

}  // namespace svp::validation::equivalence
