#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace svp::audio {

struct WhisperMelFeatures {
  std::vector<float> data;
  int n_mels = 80;
  int n_frames = 3000;
};

[[nodiscard]] WhisperMelFeatures compute_whisper_mel_from_wav(
    const std::filesystem::path& wav_path);

[[nodiscard]] std::filesystem::path slice_wav_to_temp(
    const std::filesystem::path& input_wav,
    std::int64_t start_us,
    std::int64_t end_us,
    const std::filesystem::path& temp_dir);

}  // namespace svp::audio
