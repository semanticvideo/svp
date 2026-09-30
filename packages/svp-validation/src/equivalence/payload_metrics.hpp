#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace svp::validation::equivalence {

struct DepthMetrics {
  double mean_absolute_error = 0.0;
  double p99_absolute_error = 0.0;
  double spearman = 1.0;
};

// Decoded little-endian uint16 depth payload (RC2 Section 14.4).
[[nodiscard]] std::vector<std::uint16_t> decode_uint16_le(std::span<const std::byte> bytes);

// Decoded little-endian float32 payload (RC2 Section 14.5 embedding blocks).
[[nodiscard]] std::vector<float> decode_float32_le(std::span<const std::byte> bytes);

// Depth values are normalized by kDepthUint16FullScale before comparison.
// Spearman uses average ranks for ties; when both inputs are constant their
// (empty) orderings agree and the correlation is 1, when only one is
// constant it is 0.
[[nodiscard]] DepthMetrics depth_metrics(std::span<const std::uint16_t> left,
                                         std::span<const std::uint16_t> right);

// Cosine similarity; 0 when either vector has zero norm.
[[nodiscard]] double cosine_similarity(std::span<const float> left,
                                       std::span<const float> right);

// Nearest-rank quantile of `values` (reordered in place); 0 when empty.
[[nodiscard]] double nearest_rank_quantile(std::vector<double>& values, double quantile);

}  // namespace svp::validation::equivalence
