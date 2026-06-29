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
  config.target_max_samples = 600;

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
  config.target_max_samples = 600;

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
  config.target_max_samples = 600;

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
  config.target_max_samples = 600;

  const std::int64_t duration_500ms = 500'000;
  auto result = compute_ocr_temporal_timestamps(duration_500ms, config);

  require(result.sample_count >= config.min_sample_count,
          "500ms video with 1s gap must still get min_sample_count via fallback");
  require(is_sorted_and_unique(result.timestamps_us),
          "sub-gap video timestamps must be sorted and deduped");
}

void test_long_video_gap_scaled_smoothly() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 100'000;
  config.min_sample_count = 3;
  config.target_max_samples = 50;

  const std::int64_t duration_10min = 600'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_10min, config);

  require(result.gap_scaled,
          "10min video with 100ms gap and target 50 should have gap_scaled");
  require(result.sample_count <= config.target_max_samples + 2,
          "sample_count should be near target_max_samples (plus endpoints)");
  require(result.sample_count >= 1,
          "scaled video must still have at least 1 sample");
  require(is_sorted_and_unique(result.timestamps_us),
          "scaled timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_10min),
          "scaled timestamps must be within duration");
}

void test_timestamps_sorted_deduped_within_duration() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 500'000;
  config.min_sample_count = 3;
  config.target_max_samples = 600;

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
  config.target_max_samples = 600;
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
  require(result.target_max_samples == config.target_max_samples,
          "target_max_samples must be recorded in result");
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
  require(json.contains("gap_scaled"), "JSON must have gap_scaled");
  require(json.contains("target_max_samples"), "JSON must have target_max_samples");
  require(json["sampled_timestamps_us"].is_array(), "sampled_timestamps_us must be array");
  require(json["sample_count"].get<int>() == result.sample_count,
          "JSON sample_count must match result");
}

void test_normal_duration_includes_endpoints() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.min_sample_count = 3;
  config.target_max_samples = 600;

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

void test_scaled_preserves_endpoints() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 100'000;
  config.min_sample_count = 3;
  config.target_max_samples = 50;

  const std::int64_t duration_10min = 600'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_10min, config);

  require(result.gap_scaled,
          "10min video with 100ms gap and target 50 should have gap_scaled");
  require(!result.timestamps_us.empty(), "scaled result must not be empty");
  require(result.timestamps_us.front() == 0,
          "scaled sampling must preserve timestamp 0");
  require(result.timestamps_us.back() == result.safe_end_us,
          "scaled sampling must preserve final safe_end_us timestamp");
  require(is_sorted_and_unique(result.timestamps_us),
          "scaled timestamps must be sorted and deduped");
  require(all_within_duration(result.timestamps_us, duration_10min),
          "scaled timestamps must be within duration");
}

void test_scaled_reports_effective_gap() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 100'000;
  config.min_sample_count = 3;
  config.target_max_samples = 50;

  const std::int64_t duration_10min = 600'000'000;
  auto result = compute_ocr_temporal_timestamps(duration_10min, config);

  require(result.gap_scaled, "gap should be scaled");
  require(result.effective_max_sample_gap_us > config.max_sample_gap_us,
          "effective_max_sample_gap_us must exceed configured gap when scaled");
  require(result.uncapped_sample_count > config.target_max_samples,
          "uncapped_sample_count must exceed target_max_samples");
  require(result.sparse_coverage,
          "sparse_coverage must be true when gap is scaled beyond configured");
  require(!result.sparse_coverage_reason.empty(),
          "sparse_coverage_reason must be populated");
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

void test_processor_provenance_round_trip() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.safe_end_margin_us = 100'000;

  auto result = compute_ocr_temporal_timestamps(30'000'000, config);
  auto ts_json = ocr_temporal_sampling_result_to_json(result);

  nlohmann::json processor = {
      {"id", "processor_ocr_detector_0001"},
      {"processor_type", "ocr_detector"},
      {"status", "completed"},
      {"temporal_sampling", ts_json},
  };

  std::vector<nlohmann::json> processors = {processor};

  std::string jsonl;
  for (const auto& p : processors) {
    jsonl += p.dump() + "\n";
  }

  nlohmann::json parsed = nlohmann::json::parse(jsonl.substr(0, jsonl.find('\n')));
  auto ts = parsed["temporal_sampling"];

  require(ts.contains("safe_end_us"), "round-trip temporal_sampling must have safe_end_us");
  require(ts.contains("safe_end_margin_us"), "round-trip temporal_sampling must have safe_end_margin_us");
  require(ts.contains("effective_max_sample_gap_us"), "round-trip temporal_sampling must have effective_max_sample_gap_us");
  require(ts.contains("uncapped_sample_count"), "round-trip temporal_sampling must have uncapped_sample_count");
  require(ts.contains("target_max_samples"), "round-trip temporal_sampling must have target_max_samples");
  require(ts.contains("gap_scaled"), "round-trip temporal_sampling must have gap_scaled");
  require(ts["safe_end_us"].get<std::int64_t>() == result.safe_end_us,
          "round-trip safe_end_us must match");
  require(ts["safe_end_margin_us"].get<std::int64_t>() == result.safe_end_margin_us,
          "round-trip safe_end_margin_us must match");
  require(ts["effective_max_sample_gap_us"].get<std::int64_t>() == result.effective_max_sample_gap_us,
          "round-trip effective_max_sample_gap_us must match");
  require(ts["uncapped_sample_count"].get<int>() == result.uncapped_sample_count,
          "round-trip uncapped_sample_count must match");
  require(ts["sampled_timestamps_us"].is_array(),
          "round-trip sampled_timestamps_us must be array");
  require(!ts["sampled_timestamps_us"].empty(),
          "round-trip sampled_timestamps_us must not be empty");
  require(ts["sampled_timestamps_us"][0].get<std::int64_t>() == 0,
          "round-trip first timestamp must be 0");
  require(ts["sampled_timestamps_us"].back().get<std::int64_t>() == result.safe_end_us,
          "round-trip last timestamp must be safe_end_us");
}

// --- Smooth gap scaling tests (work for any video length) ---

void test_short_video_uses_configured_gap() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  // 30s video: 30 samples at 1s gap, well under 600 target.
  auto result = compute_ocr_temporal_timestamps(30'000'000, config);

  require(!result.gap_scaled,
      "short video should not have gap_scaled");
  require(result.effective_max_sample_gap_us <= config.max_sample_gap_us,
      "effective gap should not exceed configured gap for short video");
  require(!result.sparse_coverage,
      "short video should not have sparse_coverage");
}

void test_medium_video_uses_configured_gap() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  // 548s video: 548 samples at 1s gap, under 600 target.
  auto result = compute_ocr_temporal_timestamps(548'000'000, config);

  require(!result.gap_scaled,
      "548s video with target 600 should not have gap_scaled");
  require(result.effective_max_sample_gap_us <= config.max_sample_gap_us,
      "effective gap should not exceed configured gap for 548s video");
  require(!result.sparse_coverage,
      "548s video with target 600 should not have sparse_coverage");
  require(result.sample_count > 500,
      "548s video should have ~548 samples, not capped at 120");
}

void test_long_video_scales_gap_smoothly() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  // 3600s (1 hour) video: 3600 samples at 1s gap exceeds 600 target.
  // effective_gap = max(1s, 3600s/600) = max(1s, 6s) = 6s
  auto result = compute_ocr_temporal_timestamps(3'600'000'000, config);

  require(result.gap_scaled,
      "1-hour video should have gap_scaled");
  require(result.sparse_coverage,
      "1-hour video should have sparse_coverage");
  require(result.effective_max_sample_gap_us > config.max_sample_gap_us,
      "effective gap should exceed configured gap for 1-hour video");
  require(result.sample_count <= config.target_max_samples + 2,
      "sample_count should be near target_max_samples");
  // Verify the gap is approximately 6s (6'000'000 us)
  require(result.effective_max_sample_gap_us >= 5'000'000,
      "effective gap should be approximately 6s for 3600s/600");
}

void test_very_long_video_scales_gap_smoothly() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  // 7200s (2 hour) video: effective_gap = max(1s, 7200s/600) = 12s
  auto result = compute_ocr_temporal_timestamps(7'200'000'000, config);

  require(result.gap_scaled,
      "2-hour video should have gap_scaled");
  require(result.sparse_coverage,
      "2-hour video should have sparse_coverage");
  require(result.sample_count <= config.target_max_samples + 2,
      "sample_count should be near target_max_samples even for 2-hour video");
  require(result.effective_max_sample_gap_us >= 10'000'000,
      "effective gap should be approximately 12s for 7200s/600");
  // Each quarter should still have samples (even coverage)
  const std::int64_t quarter = 7'200'000'000 / 4;
  for (int q = 0; q < 4; ++q) {
    require(has_timestamp_in_window(result.timestamps_us,
        static_cast<std::int64_t>(q) * quarter,
        static_cast<std::int64_t>(q + 1) * quarter),
        "each quarter of a 2-hour video must have at least one sample");
  }
}

void test_gap_scaling_is_continuous_no_cliff() {
  // Verify that the gap scaling is smooth — there's no duration where
  // the sample count suddenly drops.  Videos just above and below the
  // target boundary should have similar sample counts.
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  // 599s: just under target, uses 1s gap → ~599 samples
  auto r1 = compute_ocr_temporal_timestamps(599'000'000, config);
  // 601s: just over target, gap scales to ~1.002s → ~600 samples
  auto r2 = compute_ocr_temporal_timestamps(601'000'000, config);

  require(!r1.gap_scaled, "599s should not scale");
  require(r2.gap_scaled, "601s should scale");
  // The sample counts should be very close — no cliff
  const int diff = std::abs(r1.sample_count - r2.sample_count);
  require(diff <= 5,
      "sample count difference across boundary should be small (no cliff)");
}

void test_sparse_coverage_json_fields() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  auto result = compute_ocr_temporal_timestamps(3'600'000'000, config);
  auto json = ocr_temporal_sampling_result_to_json(result);

  require(json.contains("sparse_coverage"), "JSON must have sparse_coverage");
  require(json.contains("sparse_coverage_reason"), "JSON must have sparse_coverage_reason");
  require(json.contains("gap_scaled"), "JSON must have gap_scaled");
  require(json.contains("target_max_samples"), "JSON must have target_max_samples");
  require(json["sparse_coverage"].get<bool>() == true,
      "JSON sparse_coverage must be true for long video");
  require(json["gap_scaled"].get<bool>() == true,
      "JSON gap_scaled must be true for long video");
  require(!json["sparse_coverage_reason"].get<std::string>().empty(),
      "JSON sparse_coverage_reason must not be empty");
}

void test_sparse_coverage_reason_explains_scaling() {
  OcrSamplingConfig config;
  config.max_sample_gap_us = 1'000'000;
  config.target_max_samples = 600;

  auto result = compute_ocr_temporal_timestamps(3'600'000'000, config);

  require(result.sparse_coverage_reason.find("exceeds") != std::string::npos,
      "sparse_coverage_reason must explain gap exceeds configured");
  require(result.sparse_coverage_reason.find("target_max_samples") != std::string::npos,
      "sparse_coverage_reason must reference target_max_samples");
}

}  // namespace

int main() {
  test_no_hardcoded_fixed_count();
  test_34s_video_covers_18s_22s_window();
  test_short_video_sufficient_samples();
  test_short_video_below_gap_uses_min_count();
  test_long_video_gap_scaled_smoothly();
  test_timestamps_sorted_deduped_within_duration();
  test_zero_duration_returns_empty();
  test_provenance_fields_populated();
  test_json_serialization();
  test_normal_duration_includes_endpoints();
  test_scaled_preserves_endpoints();
  test_scaled_reports_effective_gap();
  test_safe_end_fields_in_json();
  test_short_duration_safe_end_equals_duration();
  test_processor_provenance_round_trip();
  test_short_video_uses_configured_gap();
  test_medium_video_uses_configured_gap();
  test_long_video_scales_gap_smoothly();
  test_very_long_video_scales_gap_smoothly();
  test_gap_scaling_is_continuous_no_cliff();
  test_sparse_coverage_json_fields();
  test_sparse_coverage_reason_explains_scaling();
  std::cout << "All OCR temporal sampling tests passed.\n";
  return 0;
}
