#include "svp/vision/ocr_temporal_sampling.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace svp::vision;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

bool is_sorted_and_unique(const std::vector<std::int64_t>& ts) {
  for (std::size_t i = 1; i < ts.size(); ++i) {
    if (ts[i] <= ts[i - 1]) return false;
  }
  return true;
}

bool all_within_duration(const std::vector<std::int64_t>& ts, std::int64_t duration_us) {
  for (const auto& t : ts) {
    if (t < 0 || t > duration_us) return false;
  }
  return true;
}

bool has_timestamp_in_window(const std::vector<std::int64_t>& ts,
                             std::int64_t window_start,
                             std::int64_t window_end) {
  for (const auto& t : ts) {
    if (t >= window_start && t <= window_end) return true;
  }
  return false;
}

void test_no_hardcoded_fixed_count() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 3;
  config.max_sample_count = 120;

  const std::int64_t duration_34s = 34'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_34s, config);

  require(result.sample_count != 5,
          "sample_count must not be hardcoded 5 for a 34s video");
  require(result.sample_count > 5,
          "34s video with 1s gap should produce more than 5 samples");
  require(result.sample_count >= config.min_sample_count,
          "sample_count must respect min_sample_count");
}

void test_34s_video_covers_18s_22s_window() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 3;
  config.max_sample_count = 120;

  const std::int64_t duration_34s = 34'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_34s, config);

  require(is_sorted_and_unique(result.timestamps_us),
          "timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_34s),
          "all timestamps must be within media duration");

  const std::int64_t window_start = 18'000'000;
  const std::int64_t window_end = 22'000'000;
  require(has_timestamp_in_window(result.timestamps_us, window_start, window_end),
          "34s video with 1s gap must have at least one sample in 18s-22s window");
}

void test_short_video_sufficient_samples() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 3;
  config.max_sample_count = 120;

  const std::int64_t duration_2s = 2'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_2s, config);

  require(result.sample_count >= config.min_sample_count,
          "2s video must still get at least min_sample_count samples");
  require(is_sorted_and_unique(result.timestamps_us),
          "short video timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_2s),
          "short video timestamps must be within duration");
}

void test_short_video_below_gap_uses_min_count() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 5;
  config.max_sample_count = 120;

  const std::int64_t duration_500ms = 500'000;
  auto result = compute_ocr_temporal_timestamps(duration_500ms, config);

  require(result.sample_count >= config.min_sample_count,
          "500ms video with 1s gap must still get min_sample_count via fallback");
  require(is_sorted_and_unique(result.timestamps_us),
          "sub-gap video timestamps must be sorted and deduped");
}

void test_long_video_cap_applied() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 100'000;
  config.min_sample_count = 3;
  config.max_sample_count = 50;

  const std::int64_t duration_10min = 600'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_10min, config);

  require(result.cap_applied,
          "10min video with 100ms gap and max 50 should have cap applied");
  require(result.sample_count <= config.max_sample_count,
          "sample_count must not exceed max_sample_count");
  require(result.sample_count >= 1,
          "capped video must still have at least 1 sample");
  require(is_sorted_and_unique(result.timestamps_us),
          "capped timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_10min),
          "capped timestamps must be within duration");
}

void test_timestamps_sorted_deduped_within_duration() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 500'000;
  config.min_sample_count = 3;
  config.max_sample_count = 200;

  const std::int64_t duration_60s = 60'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_60s, config);

  require(is_sorted_and_unique(result.timestamps_us),
          "60s video timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_60s),
          "60s video timestamps must be within duration");
  require(result.sample_count > 0,
          "60s video must produce at least 1 sample");
}

void test_zero_duration_returns_empty() {
  OcrSamplingConfig config;
  auto result = compute_ocr_temporal_timestamps(0, config);

  require(result.timestamps_us.empty(),
          "zero duration should produce no timestamps");
  require(result.sample_count == 0,
          "zero duration sample_count should be 0");
}

void test_provenance_fields_populated() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 3;
  config.max_sample_count = 120;
  config.sampling_strategy = "temporal_interval";
  config.temporal_coverage_note = "Text visible for less than max_sample_gap_us may be missed.";

  const std::int64_t duration_30s = 30'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_30s, config);

  require(result.sampling_strategy == "temporal_interval",
          "sampling_strategy must be populated");
  require(!result.temporal_coverage_note.empty(),
          "temporal_coverage_note must be populated");
  require(result.duration_us == duration_30s,
          "duration_us must be recorded in result");
  require(result.max_sample_gap_us == config.max_sample_gap_us,
          "max_sample_gap_us must be recorded in result");
  require(result.sample_count == static_cast<int>(result.timestamps_us.size()),
          "sample_count must match timestamps size");
}

void test_json_serialization() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;

  auto result = compute_ocr_temporal_timestamps(30'000'000, config);
  auto json = ocr_temporal_sampling_result_to_json(result);

  require(json.contains("sampling_strategy"), "JSON must have sampling_strategy");
  require(json.contains("duration_us"), "JSON must have duration_us");
  require(json.contains("max_sample_gap_us"), "JSON must have max_sample_gap_us");
  require(json.contains("sample_count"), "JSON must have sample_count");
  require(json.contains("sampled_timestamps_us"), "JSON must have sampled_timestamps_us");
  require(json.contains("temporal_coverage_note"), "JSON must have temporal_coverage_note");
  require(json.contains("cap_applied"), "JSON must have cap_applied");
  require(json["sampled_timestamps_us"].is_array(), "sampled_timestamps_us must be array");
  require(json["sample_count"].get<int>() == result.sample_count,
          "JSON sample_count must match result");
}

void test_normal_duration_includes_endpoints() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 3;
  config.max_sample_count = 120;

  const std::int64_t duration_30s = 30'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_30s, config);

  require(!result.timestamps_us.empty(), "30s video must produce timestamps");
  require(result.timestamps_us.front() == 0,
          "first timestamp must be 0");
  require(result.timestamps_us.back() == result.safe_end_us,
          "last timestamp must equal safe_end_us");
  require(result.safe_end_us == duration_30s - config.safe_end_margin_us,
          "safe_end_us must be duration - margin");
  require(result.safe_end_margin_us == config.safe_end_margin_us,
          "safe_end_margin_us must be recorded");
}

void test_capped_preserves_endpoints() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 100'000;
  config.min_sample_count = 3;
  config.max_sample_count = 50;

  const std::int64_t duration_10min = 600'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_10min, config);

  require(result.cap_applied,
          "10min video with 100ms gap and max 50 should have cap applied");
  require(!result.timestamps_us.empty(), "capped result must not be empty");
  require(result.timestamps_us.front() == 0,
          "capped sampling must preserve timestamp 0");
  require(result.timestamps_us.back() == result.safe_end_us,
          "capped sampling must preserve final safe_end_us timestamp");
  require(result.sample_count <= config.max_sample_count,
          "sample_count must not exceed max_sample_count");
  require(is_sorted_and_unique(result.timestamps_us),
          "capped timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_10min),
          "capped timestamps must be within duration");
}

void test_capped_reports_effective_gap() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 100'000;
  config.min_sample_count = 3;
  config.max_sample_count = 50;

  const std::int64_t duration_10min = 600'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_10min, config);

  require(result.cap_applied, "cap should be applied");
  require(result.effective_max_sample_gap_us > config.max_sample_gap_us,
          "effective_max_sample_gap_us must exceed configured gap under cap");
  require(result.uncapped_sample_count > config.max_sample_count,
          "uncapped_sample_count must exceed max_sample_count");
  require(result.temporal_coverage_note.find("Sampling cap applied") != std::string::npos,
          "coverage note must mention cap when effective gap exceeds configured");
  require(result.temporal_coverage_note.find("effective_max_sample_gap_us") != std::string::npos,
          "coverage note must reference effective_max_sample_gap_us");
}

void test_safe_end_fields_in_json() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.safe_end_margin_us = 100'000;

  auto result = compute_ocr_temporal_timestamps(30'000'000, config);
  auto json = ocr_temporal_sampling_result_to_json(result);

  require(json.contains("safe_end_margin_us"), "JSON must have safe_end_margin_us");
  require(json.contains("safe_end_us"), "JSON must have safe_end_us");
  require(json.contains("effective_max_sample_gap_us"), "JSON must have effective_max_sample_gap_us");
  require(json.contains("uncapped_sample_count"), "JSON must have uncapped_sample_count");
  require(json["safe_end_margin_us"].get<std::int64_t>() == config.safe_end_margin_us,
          "JSON safe_end_margin_us must match config");
  require(json["safe_end_us"].get<std::int64_t>() == result.safe_end_us,
          "JSON safe_end_us must match result");
}

void test_short_duration_safe_end_equals_duration() {
  OcrSamplingConfig config;
  config.safe_end_margin_us = 100'000;

  const std::int64_t duration_50ms = 50'000;
  auto result = compute_ocr_temporal_timestamps(duration_50ms, config);

  require(result.safe_end_us == duration_50ms,
          "when duration < margin, safe_end_us must equal duration");
}

}  // namespace

int main() {
  test_no_hardcoded_fixed_count();
  test_34s_video_covers_18s_22s_window();
  test_short_video_sufficient_samples();
  test_short_video_below_gap_uses_min_count();
  test_long_video_cap_applied();
  test_timestamps_sorted_deduped_within_duration();
  test_zero_duration_returns_empty();
  test_provenance_fields_populated();
  test_json_serialization();
  test_normal_duration_includes_endpoints();
  test_capped_preserves_endpoints();
  test_capped_reports_effective_gap();
  test_safe_end_fields_in_json();
  test_short_duration_safe_end_equals_duration();
  std::cout << "All OCR temporal sampling tests passed.\n";
  return 0;
}
