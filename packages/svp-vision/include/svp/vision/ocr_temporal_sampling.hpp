#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace svp::vision {

struct OcrSamplingConfig {
  // The desired sampling gap. For short videos this gap is used as-is.
  // For longer videos, the effective gap may grow to keep total samples
  // near target_max_samples, but never below this configured gap.
  std::int64_t max_sample_gap_us = 1'000'000;
  std::int64_t safe_end_margin_us = 100'000;
  int min_sample_count = 3;

  // Soft target for total sample count. Instead of a hard cap that creates
  // a coverage cliff, the effective sampling gap is computed as:
  //   effective_gap = max(max_sample_gap_us, duration / target_max_samples)
  // This scales smoothly for any video length. When the effective gap
  // exceeds max_sample_gap_us, sparse_coverage provenance is reported.
  int target_max_samples = 600;

  std::string sampling_strategy = "temporal_interval";
  std::string temporal_coverage_note =
      "Text visible for less than max_sample_gap_us may be missed.";
};

struct OcrTemporalSamplingResult {
  std::vector<std::int64_t> timestamps_us;
  std::int64_t duration_us = 0;
  std::int64_t max_sample_gap_us = 0;
  std::int64_t safe_end_margin_us = 0;
  std::int64_t safe_end_us = 0;
  std::int64_t effective_max_sample_gap_us = 0;
  int sample_count = 0;
  int uncapped_sample_count = 0;
  int min_sample_count = 0;
  int target_max_samples = 0;
  bool gap_scaled = false;
  std::string sampling_strategy;
  std::string temporal_coverage_note;

  // Coverage provenance for sparse coverage reporting
  bool sparse_coverage = false;
  std::string sparse_coverage_reason;
};

[[nodiscard]] OcrTemporalSamplingResult compute_ocr_temporal_timestamps(
    std::int64_t duration_us,
    const OcrSamplingConfig& config);

[[nodiscard]] nlohmann::json ocr_temporal_sampling_result_to_json(
    const OcrTemporalSamplingResult& result);

}  // namespace svp::vision
