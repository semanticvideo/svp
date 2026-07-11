#pragma once

#include <filesystem>
#include <vector>

namespace svp::audio {

[[nodiscard]] std::vector<float> read_whisper_pcm16_mono_wav(
    const std::filesystem::path& wav_path);

}  // namespace svp::audio
