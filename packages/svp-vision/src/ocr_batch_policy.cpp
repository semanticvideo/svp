#include "svp/vision/ocr_batch_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace svp::vision {

void validate_ocr_batch_policy(const OcrBatchPolicy& policy) {
  if (!std::isfinite(policy.target_task_seconds) ||
      policy.target_task_seconds <= 0.0) {
    throw std::invalid_argument(
        "OcrBatchPolicy.target_task_seconds must be finite and > 0");
  }
  if (!std::isfinite(policy.estimated_seconds_per_sample) ||
      policy.estimated_seconds_per_sample <= 0.0) {
    throw std::invalid_argument(
        "OcrBatchPolicy.estimated_seconds_per_sample must be finite and > 0");
  }
}

std::uint64_t ocr_batch_sample_count(const OcrBatchPolicy& policy) {
  validate_ocr_batch_policy(policy);
  const double samples =
      std::floor(policy.target_task_seconds / policy.estimated_seconds_per_sample);
  // A ratio too large for uint64 still means "everything in one batch".
  constexpr auto kMaxCount = std::numeric_limits<std::uint64_t>::max();
  if (!(samples < static_cast<double>(kMaxCount))) {
    return kMaxCount;
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(samples));
}

std::vector<OcrSampleBatch> partition_ocr_samples(std::size_t sample_count,
                                                  const OcrBatchPolicy& policy) {
  const std::uint64_t per_batch = ocr_batch_sample_count(policy);
  const auto total = static_cast<std::uint64_t>(sample_count);
  std::vector<OcrSampleBatch> batches;
  for (std::uint64_t first = 0; first < total;) {
    const std::uint64_t count = std::min(per_batch, total - first);
    batches.push_back(OcrSampleBatch{.first_ordinal = first, .count = count});
    first += count;
  }
  return batches;
}

}  // namespace svp::vision
