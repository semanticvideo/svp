#pragma once

#include <cstdint>
#include <filesystem>

namespace svp::audio {

[[nodiscard]] std::filesystem::path slice_wav_to_temp(
    const std::filesystem::path& input_wav,
    std::int64_t start_us,
    std::int64_t end_us,
    const std::filesystem::path& temp_dir);

}  // namespace svp::audio
