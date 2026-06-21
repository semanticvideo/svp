#pragma once

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

}  // namespace svp::audio
