#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace svp::validation::equivalence {

// Binary mask plane in row-major order, 1 = foreground.
struct MaskPlane {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool empty() const noexcept;
};

// Decodes a mask block payload into `plane_count` planes. Supports RC2
// Section 14.3 svp-rle-v1 (unsigned LEB128 runs, starting with background)
// and Section 14.5 bitpacked_lsb_first. Returns nullopt when the payload
// does not decode to exactly width * height * plane_count pixels.
[[nodiscard]] std::optional<std::vector<MaskPlane>> decode_mask_planes(
    std::span<const std::byte> payload,
    std::uint32_t dtype,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t plane_count);

struct MaskMetrics {
  double iou = 1.0;
  // Quantile (DefaultEquivalenceProfileV1::kMaskBoundaryDisplacementQuantile)
  // of symmetric distances, in pixels, from each boundary pixel of one mask
  // to the nearest boundary pixel of the other.
  double boundary_displacement_px = 0.0;
};

// Both planes must be non-empty and have equal dimensions.
[[nodiscard]] MaskMetrics mask_metrics(const MaskPlane& left, const MaskPlane& right);

}  // namespace svp::validation::equivalence
