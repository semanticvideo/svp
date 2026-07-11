#pragma once

#include "svp/audio/whisper_model.hpp"

#include <filesystem>

namespace svp::audio {

[[nodiscard]] bool is_whisper_cpp_runtime_available() noexcept;

void set_whisper_cpp_verbose(bool verbose) noexcept;

void release_whisper_cpp_model() noexcept;

[[nodiscard]] WhisperInferenceResult run_whisper_cpp_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& ggml_model_path,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us);

}  // namespace svp::audio
