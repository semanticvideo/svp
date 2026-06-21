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
  result.max_sample_count = config.max_sample_count;
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

  result.uncapped_sample_count = static_cast<int>(timestamps.size());

  if (config.max_sample_count > 0 &&
      static_cast<int>(timestamps.size()) > config.max_sample_count) {
    result.cap_applied = true;
    const int total = static_cast<int>(timestamps.size());
    const int cap = config.max_sample_count;
    std::vector<std::int64_t> capped;
    capped.reserve(static_cast<std::size_t>(cap));

    // Endpoint-preserving subsampling: always include index 0 and index total-1,
    // then evenly space the remaining cap-2 slots between them.
    if (cap >= 2) {
      capped.push_back(timestamps[0]);
      for (int i = 1; i < cap - 1; ++i) {
        const int src_idx = static_cast<int>(
            static_cast<std::int64_t>(i) * (total - 1) / (cap - 1));
        capped.push_back(timestamps[static_cast<std::size_t>(src_idx)]);
      }
      capped.push_back(timestamps[static_cast<std::size_t>(total - 1)]);
    } else {
      capped.push_back(timestamps[0]);
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

  // Compute effective max gap from the actual timestamp list.
  std::int64_t effective_gap = 0;
  for (std::size_t i = 1; i < timestamps.size(); ++i) {
    const std::int64_t diff = timestamps[i] - timestamps[i - 1];
    if (diff > effective_gap) effective_gap = diff;
  }
  if (timestamps.size() <= 1 && !timestamps.empty()) {
    effective_gap = timestamps[0];
  }
  result.effective_max_sample_gap_us = effective_gap;

  // Update coverage note if cap expanded the effective gap beyond configured.
  if (result.cap_applied && effective_gap > config.max_sample_gap_us) {
    result.temporal_coverage_note =
        "Sampling cap applied; effective_max_sample_gap_us is " +
        std::to_string(effective_gap) +
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
      {"max_sample_count", result.max_sample_count},
      {"cap_applied", result.cap_applied},
      {"sampled_timestamps_us", ts_arr},
      {"temporal_coverage_note", result.temporal_coverage_note},
  };
}

}  // namespace svp::vision
