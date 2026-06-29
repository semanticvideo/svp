#include "svp/vision/ocr_temporal_sampling.hpp"

#include <algorithm>
#include <cstdint>

namespace svp::vision {

OcrTemporalSamplingResult compute_ocr_temporal_timestamps(
    std::int64_t duration_us,
    const OcrSamplingConfig& config) {
  OcrTemporalSamplingResult result;
  result.duration_us = duration_us;
  result.max_sample_gap_us = config.max_sample_gap_us;
  result.safe_end_margin_us = config.safe_end_margin_us;
  result.min_sample_count = config.min_sample_count;
  result.target_max_samples = config.target_max_samples;
  result.sampling_strategy = config.sampling_strategy;
  result.temporal_coverage_note = config.temporal_coverage_note;

  if (duration_us <= 0) {
    return result;
  }

  const std::int64_t margin = config.safe_end_margin_us > 0
      ? config.safe_end_margin_us
      : 100'000;
  const std::int64_t safe_end =
      (duration_us > margin) ? (duration_us - margin) : duration_us;
  result.safe_end_us = safe_end;
  result.safe_end_margin_us = margin;

  const std::int64_t configured_gap = config.max_sample_gap_us > 0
      ? config.max_sample_gap_us
      : 1'000'000;

  // Compute the effective sampling gap using smooth proportional scaling.
  // effective_gap = max(configured_gap, safe_end / target_max_samples)
  //
  // This replaces the old hard max_sample_count cap.  For short videos
  // the configured gap is used as-is.  For longer videos the gap grows
  // smoothly so that total samples stays near target_max_samples.
  // There is no cliff or threshold — the scaling is continuous for any
  // video duration.
  std::int64_t effective_gap = configured_gap;
  if (config.target_max_samples > 0) {
    const std::int64_t scaled_gap = safe_end / config.target_max_samples;
    if (scaled_gap > configured_gap) {
      effective_gap = scaled_gap;
      result.gap_scaled = true;
    }
  }

  // Generate timestamps at the effective gap.
  std::vector<std::int64_t> timestamps;
  timestamps.push_back(0);

  for (std::int64_t t = effective_gap; t < safe_end; t += effective_gap) {
    timestamps.push_back(t);
  }

  if (timestamps.back() < safe_end) {
    timestamps.push_back(safe_end);
  }

  // Ensure minimum sample count for very short videos.
  if (static_cast<int>(timestamps.size()) < config.min_sample_count &&
      config.min_sample_count > 0) {
    timestamps.clear();
    for (int i = 0; i < config.min_sample_count; ++i) {
      const std::int64_t ts =
          safe_end * (2 * i + 1) / (2 * config.min_sample_count);
      timestamps.push_back(ts);
    }
  }

  // uncapped_sample_count = how many samples we'd get at the configured gap,
  // including the safe_end endpoint if it doesn't fall exactly on the gap.
  {
    std::int64_t uncapped_count = 1;  // timestamp 0
    for (std::int64_t t = configured_gap; t < safe_end; t += configured_gap) {
      uncapped_count++;
    }
    // Check if safe_end would be appended as an extra endpoint
    if (safe_end % configured_gap != 0) {
      uncapped_count++;
    }
    result.uncapped_sample_count = static_cast<int>(uncapped_count);
  }

  std::sort(timestamps.begin(), timestamps.end());
  timestamps.erase(
      std::unique(timestamps.begin(), timestamps.end()),
      timestamps.end());

  for (auto& t : timestamps) {
    if (t > safe_end) t = safe_end;
    if (t < 0) t = 0;
  }

  // Compute the actual effective max gap from the timestamp list.
  std::int64_t actual_effective_gap = 0;
  for (std::size_t i = 1; i < timestamps.size(); ++i) {
    const std::int64_t diff = timestamps[i] - timestamps[i - 1];
    if (diff > actual_effective_gap) actual_effective_gap = diff;
  }
  if (timestamps.size() <= 1 && !timestamps.empty()) {
    actual_effective_gap = timestamps[0];
  }
  result.effective_max_sample_gap_us = actual_effective_gap;

  // Report sparse coverage when the gap was scaled beyond the configured gap.
  if (result.gap_scaled && actual_effective_gap > configured_gap) {
    result.sparse_coverage = true;
    result.sparse_coverage_reason =
        "Effective sampling gap (" + std::to_string(actual_effective_gap) +
        " us) exceeds configured max_sample_gap_us (" +
        std::to_string(configured_gap) +
        " us) because video duration (" + std::to_string(duration_us) +
        " us) with target_max_samples (" +
        std::to_string(config.target_max_samples) +
        ") requires a larger gap to bound total samples. "
        "Text visible for less than the effective gap may be missed.";
    result.temporal_coverage_note =
        "Sparse coverage: effective_max_sample_gap_us is " +
        std::to_string(actual_effective_gap) +
        ", so text visible for less than that effective gap may be missed.";
  }

  result.timestamps_us = std::move(timestamps);
  result.sample_count = static_cast<int>(result.timestamps_us.size());
  return result;
}

nlohmann::json ocr_temporal_sampling_result_to_json(
    const OcrTemporalSamplingResult& result) {
  nlohmann::json ts_arr = nlohmann::json::array();
  for (const auto& ts : result.timestamps_us) {
    ts_arr.push_back(ts);
  }
  return {
      {"sampling_strategy", result.sampling_strategy},
      {"duration_us", result.duration_us},
      {"max_sample_gap_us", result.max_sample_gap_us},
      {"safe_end_margin_us", result.safe_end_margin_us},
      {"safe_end_us", result.safe_end_us},
      {"effective_max_sample_gap_us", result.effective_max_sample_gap_us},
      {"sample_count", result.sample_count},
      {"uncapped_sample_count", result.uncapped_sample_count},
      {"min_sample_count", result.min_sample_count},
      {"target_max_samples", result.target_max_samples},
      {"gap_scaled", result.gap_scaled},
      {"sampled_timestamps_us", ts_arr},
      {"temporal_coverage_note", result.temporal_coverage_note},
      {"sparse_coverage", result.sparse_coverage},
      {"sparse_coverage_reason", result.sparse_coverage_reason},
  };
}

}  // namespace svp::vision
