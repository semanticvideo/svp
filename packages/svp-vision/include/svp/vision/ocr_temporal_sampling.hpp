#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace svp::vision {

struct OcrSamplingConfig {
  std::int64_t max_sample_gap_us = 1'000'000;
  int min_sample_count = 3;
  int max_sample_count = 120;
  std::string sampling_strategy = "temporal_interval";
  std::string temporal_coverage_note =
      "Text visible for less than max_sample_gap_us may be missed.";
};

struct OcrTemporalSamplingResult {
  std::vector<std::int64_t> timestamps_us;
  std::int64_t duration_us = 0;
  std::int64_t max_sample_gap_us = 0;
  int sample_count = 0;
  int min_sample_count = 0;
  int max_sample_count = 0;
  bool cap_applied = false;
  std::string sampling_strategy;
  std::string temporal_coverage_note;
};

[[nodiscard]] OcrTemporalSamplingResult compute_ocr_temporal_timestamps(
    std::int64_t duration_us,
    const OcrSamplingConfig& config);

[[nodiscard]] nlohmann::json ocr_temporal_sampling_result_to_json(
    const OcrTemporalSamplingResult& result);

}  // namespace svp::vision
