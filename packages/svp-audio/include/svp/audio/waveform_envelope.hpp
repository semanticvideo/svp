#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace svp::audio {

struct WaveformEnvelopeRecord {
  std::int64_t index = 0;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  double rms_db = 0.0;
  double peak_db = 0.0;
};

[[nodiscard]] std::vector<WaveformEnvelopeRecord> generate_waveform_envelope_records(
    const std::filesystem::path& analysis_wav_path,
    std::int64_t window_duration_us);

[[nodiscard]] nlohmann::json waveform_envelope_record_to_json(
    const WaveformEnvelopeRecord& record);

}  // namespace svp::audio
