#include "equivalence/payload_metrics.hpp"

#include "equivalence/equivalence_profile.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>

namespace svp::validation::equivalence {
namespace {

constexpr std::size_t kUint16Values = std::numeric_limits<std::uint16_t>::max() + 1U;

// Average 1-based rank of every uint16 value (counting sort, O(n + 65536)).
std::vector<double> average_ranks(std::span<const std::uint16_t> values) {
  std::vector<std::uint64_t> counts(kUint16Values, 0);
  for (const auto value : values) {
    ++counts[value];
  }
  std::vector<double> rank_of_value(kUint16Values, 0.0);
  std::uint64_t below = 0;
  for (std::size_t value = 0; value < kUint16Values; ++value) {
    if (counts[value] > 0) {
      // Ranks below+1 .. below+count share their mean.
      rank_of_value[value] =
          static_cast<double>(below) + (static_cast<double>(counts[value]) + 1.0) / 2.0;
      below += counts[value];
    }
  }
  std::vector<double> ranks(values.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    ranks[index] = rank_of_value[values[index]];
  }
  return ranks;
}

bool is_constant(std::span<const std::uint16_t> values) {
  return std::adjacent_find(values.begin(), values.end(), std::not_equal_to<>()) ==
         values.end();
}

double pearson(const std::vector<double>& left, const std::vector<double>& right) {
  const auto count = static_cast<double>(left.size());
  double left_mean = 0.0;
  double right_mean = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    left_mean += left[index];
    right_mean += right[index];
  }
  left_mean /= count;
  right_mean /= count;
  double covariance = 0.0;
  double left_variance = 0.0;
  double right_variance = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const double left_delta = left[index] - left_mean;
    const double right_delta = right[index] - right_mean;
    covariance += left_delta * right_delta;
    left_variance += left_delta * left_delta;
    right_variance += right_delta * right_delta;
  }
  return covariance / std::sqrt(left_variance * right_variance);
}

}  // namespace

std::vector<std::uint16_t> decode_uint16_le(std::span<const std::byte> bytes) {
  std::vector<std::uint16_t> values(bytes.size() / sizeof(std::uint16_t));
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto low = std::to_integer<std::uint16_t>(bytes[index * 2]);
    const auto high = std::to_integer<std::uint16_t>(bytes[index * 2 + 1]);
    values[index] = static_cast<std::uint16_t>(low | (high << 8U));
  }
  return values;
}

std::vector<float> decode_float32_le(std::span<const std::byte> bytes) {
  std::vector<float> values(bytes.size() / sizeof(float));
  for (std::size_t index = 0; index < values.size(); ++index) {
    std::uint32_t bits = 0;
    for (std::size_t byte = 0; byte < sizeof(float); ++byte) {
      bits |= std::to_integer<std::uint32_t>(bytes[index * sizeof(float) + byte])
              << (8U * byte);
    }
    values[index] = std::bit_cast<float>(bits);
  }
  return values;
}

double nearest_rank_quantile(std::vector<double>& values, double quantile) {
  if (values.empty()) {
    return 0.0;
  }
  const auto rank = static_cast<std::size_t>(
      std::ceil(quantile * static_cast<double>(values.size())));
  const auto index = std::min(values.size() - 1, rank == 0 ? 0 : rank - 1);
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                   values.end());
  return values[index];
}

DepthMetrics depth_metrics(std::span<const std::uint16_t> left,
                           std::span<const std::uint16_t> right) {
  DepthMetrics metrics;
  if (left.empty()) {
    return metrics;
  }

  std::vector<double> errors(left.size());
  double error_sum = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    errors[index] = std::fabs(static_cast<double>(left[index]) -
                              static_cast<double>(right[index])) /
                    kDepthUint16FullScale;
    error_sum += errors[index];
  }
  metrics.mean_absolute_error = error_sum / static_cast<double>(left.size());
  metrics.p99_absolute_error =
      nearest_rank_quantile(errors, DefaultEquivalenceProfileV1::kDepthP99Quantile);

  const bool left_constant = is_constant(left);
  const bool right_constant = is_constant(right);
  if (left_constant || right_constant) {
    metrics.spearman = left_constant && right_constant ? 1.0 : 0.0;
  } else {
    metrics.spearman = pearson(average_ranks(left), average_ranks(right));
  }
  return metrics;
}

double cosine_similarity(std::span<const float> left, std::span<const float> right) {
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  for (std::size_t index = 0; index < left.size() && index < right.size(); ++index) {
    dot += static_cast<double>(left[index]) * static_cast<double>(right[index]);
    left_norm += static_cast<double>(left[index]) * static_cast<double>(left[index]);
    right_norm += static_cast<double>(right[index]) * static_cast<double>(right[index]);
  }
  if (left_norm == 0.0 || right_norm == 0.0) {
    return 0.0;
  }
  return dot / std::sqrt(left_norm * right_norm);
}

}  // namespace svp::validation::equivalence
