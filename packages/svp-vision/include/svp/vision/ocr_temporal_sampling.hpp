#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
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

// Environment variable that replaces the OCR sample schedule with an explicit
// comma-separated list of microsecond timestamps. Diagnostic only; never a
// production coverage claim.
inline constexpr const char* kOcrDiagnosticTimestampsEnv =
    "SVP_OCR_DIAG_TIMESTAMPS_US";

// Parses a diagnostic timestamp list ("0,1500000,..."). Returns nullopt for
// null, empty, negative, or malformed input. The result is sorted and unique.
[[nodiscard]] std::optional<std::vector<std::int64_t>>
parse_ocr_diagnostic_timestamps(const char* value);

// Reads kOcrDiagnosticTimestampsEnv from the process environment.
[[nodiscard]] std::optional<std::vector<std::int64_t>>
ocr_diagnostic_timestamp_override();

// The OCR sample schedule actually used by OCR generation: the temporal
// schedule for the duration, replaced by the diagnostic override when one is
// supplied. Shared by OCR generation and the frame plan so both agree.
[[nodiscard]] OcrTemporalSamplingResult plan_ocr_temporal_sampling(
    std::int64_t duration_us,
    const OcrSamplingConfig& config,
    const std::optional<std::vector<std::int64_t>>& diagnostic_override);

[[nodiscard]] nlohmann::json ocr_temporal_sampling_result_to_json(
    const OcrTemporalSamplingResult& result);

}  // namespace svp::vision
