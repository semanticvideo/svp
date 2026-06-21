#include "svp/vision/ocr_temporal_sampling.hpp"

#include <algorithm>
#include <cstdint>

namespace svp::vision {

namespace {

constexpr std::int64_t kEndMarginUs = 100'000;

}  // namespace

OcrTemporalSamplingResult compute_ocr_temporal_timestamps(
    std::int64_t duration_us,
    const OcrSamplingConfig& config) {
  OcrTemporalSamplingResult result;
  result.duration_us = duration_us;
  result.max_sample_gap_us = config.max_sample_gap_us;
  result.min_sample_count = config.min_sample_count;
  result.max_sample_count = config.max_sample_count;
  result.sampling_strategy = config.sampling_strategy;
  result.temporal_coverage_note = config.temporal_coverage_note;

  if (duration_us <= 0) {
    return result;
  }

  const std::int64_t safe_end =
      (duration_us > kEndMarginUs) ? (duration_us - kEndMarginUs) : duration_us;

  const std::int64_t gap = config.max_sample_gap_us > 0
      ? config.max_sample_gap_us
      : 1'000'000;

  std::vector<std::int64_t> timestamps;
  timestamps.push_back(0);

  for (std::int64_t t = gap; t < safe_end; t += gap) {
    timestamps.push_back(t);
  }

  if (timestamps.back() < safe_end) {
    timestamps.push_back(safe_end);
  }

  if (static_cast<int>(timestamps.size()) < config.min_sample_count &&
      config.min_sample_count > 0) {
    timestamps.clear();
    for (int i = 0; i < config.min_sample_count; ++i) {
      const std::int64_t ts =
          safe_end * (2 * i + 1) / (2 * config.min_sample_count);
      timestamps.push_back(ts);
    }
  }

  if (config.max_sample_count > 0 &&
      static_cast<int>(timestamps.size()) > config.max_sample_count) {
    result.cap_applied = true;
    std::vector<std::int64_t> capped;
    capped.reserve(static_cast<std::size_t>(config.max_sample_count));
    const int total = static_cast<int>(timestamps.size());
    for (int i = 0; i < config.max_sample_count; ++i) {
      const int src_idx = static_cast<int>(
          static_cast<std::int64_t>(i) * total / config.max_sample_count);
      capped.push_back(timestamps[static_cast<std::size_t>(src_idx)]);
    }
    timestamps = std::move(capped);
  }

  std::sort(timestamps.begin(), timestamps.end());
  timestamps.erase(
      std::unique(timestamps.begin(), timestamps.end()),
      timestamps.end());

  for (auto& t : timestamps) {
    if (t > safe_end) t = safe_end;
    if (t < 0) t = 0;
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
      {"sample_count", result.sample_count},
      {"min_sample_count", result.min_sample_count},
      {"max_sample_count", result.max_sample_count},
      {"cap_applied", result.cap_applied},
      {"sampled_timestamps_us", ts_arr},
      {"temporal_coverage_note", result.temporal_coverage_note},
  };
}

}  // namespace svp::vision
