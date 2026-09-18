#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::audio {

// Spectrum observations share the 400 ms grid used by loudness so records in
// the two layers join by index and timing without re-sampling.
inline constexpr std::int64_t kSpectrumWindowDurationUs = 400000;

// Ten IEC 61260 octave-band center frequencies in Hz. Each band spans
// [center/sqrt(2), center*sqrt(2)); edges past a stream's Nyquist frequency are
// truncated, and bands entirely above Nyquist report null.
inline constexpr std::size_t kSpectrumBandCount = 10;
inline constexpr std::array<double, kSpectrumBandCount> kSpectrumBandCentersHz = {
    31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

struct SpectrumWindowRecord {
  std::int64_t index = 0;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::array<std::optional<double>, kSpectrumBandCount> bands_dbfs{};
};

struct SpectrumStreamSummary {
  std::string target_id;
  std::array<std::optional<double>, kSpectrumBandCount> mean_band_dbfs{};
  std::array<std::optional<double>, kSpectrumBandCount> max_band_dbfs{};
  std::int32_t channels = 0;
  std::int32_t sample_rate = 0;
};

struct SpectrumMeasurement {
  std::vector<SpectrumWindowRecord> windows;
  SpectrumStreamSummary summary;
};

// Pure analysis core: measures octave-band energy on interleaved float PCM at
// the stream's native rate. A full-scale sine reads 0 dBFS in its containing
// band; bands with no measurable energy are nullopt.
[[nodiscard]] SpectrumMeasurement measure_spectrum_pcm(
    const float* interleaved_pcm,
    std::int64_t frame_count,
    std::int32_t channels,
    std::int32_t sample_rate,
    std::int64_t stream_start_offset_us,
    const std::string& target_id);

// Decodes an audio file to float PCM through ffmpeg and measures it. The file
// keeps its native sample rate and channel layout.
[[nodiscard]] SpectrumMeasurement measure_spectrum(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& audio_path,
    std::int64_t stream_start_offset_us,
    std::int32_t sample_rate,
    std::int32_t channels,
    const std::string& target_id);

[[nodiscard]] nlohmann::json spectrum_window_record_to_json(
    const SpectrumWindowRecord& record,
    std::string_view target_id,
    std::string_view processor_id);

[[nodiscard]] nlohmann::json spectrum_summary_to_json(
    const std::vector<SpectrumStreamSummary>& streams,
    std::int64_t window_us,
    std::string_view processor_id);

}  // namespace svp::audio
