#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::audio {

// Measurement window length is the ITU-R BS.1770 momentary block length.
inline constexpr std::int64_t kLoudnessWindowDurationUs = 400000;

struct LoudnessWindowRecord {
  std::int64_t index = 0;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::optional<double> momentary_lufs;
  std::optional<double> shortterm_lufs;
  std::optional<double> true_peak_dbtp;
};

struct LoudnessStreamSummary {
  std::string target_id;
  std::optional<double> integrated_lufs;
  std::optional<double> loudness_range_lu;
  std::optional<double> lra_low_lufs;
  std::optional<double> lra_high_lufs;
  std::optional<double> true_peak_dbtp;
  std::optional<double> max_momentary_lufs;
  std::optional<double> max_shortterm_lufs;
  std::int32_t channels = 0;
  std::int32_t sample_rate = 0;
};

struct LoudnessMeasurement {
  std::vector<LoudnessWindowRecord> windows;
  LoudnessStreamSummary summary;
};

[[nodiscard]] LoudnessMeasurement measure_loudness(
    const std::filesystem::path& ffprobe_path,
    const std::filesystem::path& audio_path,
    std::int64_t window_duration_us,
    std::int64_t stream_start_offset_us,
    std::int32_t sample_rate,
    std::int32_t channels,
    const std::string& target_id);

[[nodiscard]] nlohmann::json loudness_window_record_to_json(
    const LoudnessWindowRecord& record,
    std::string_view target_id,
    std::string_view processor_id);

[[nodiscard]] nlohmann::json loudness_summary_to_json(
    const std::vector<LoudnessStreamSummary>& streams,
    std::int64_t window_us,
    std::string_view processor_id);

}  // namespace svp::audio
