#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace svp::audio {

// Throws std::runtime_error when [start_us, end_us) holds no samples of
// `input_wav`.
[[nodiscard]] std::filesystem::path slice_wav_to_temp(
    const std::filesystem::path& input_wav,
    std::int64_t start_us,
    std::int64_t end_us,
    const std::filesystem::path& temp_dir);

// The same, but nullopt when [start_us, end_us) holds no samples of
// `input_wav` (it lies at or past the end of the audio).
[[nodiscard]] std::optional<std::filesystem::path> slice_wav_range_to_temp(
    const std::filesystem::path& input_wav,
    std::int64_t start_us,
    std::int64_t end_us,
    const std::filesystem::path& temp_dir);

}  // namespace svp::audio
