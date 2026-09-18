#pragma once

#include "svp/audio/whisper_model.hpp"

#include <filesystem>

namespace svp::audio {

[[nodiscard]] bool is_whisper_cpp_runtime_available() noexcept;

void set_whisper_cpp_verbose(bool verbose) noexcept;

void release_whisper_cpp_model() noexcept;

void release_phoneme_aligner() noexcept;

[[nodiscard]] WhisperInferenceResult run_whisper_cpp_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& ggml_model_path,
    const std::filesystem::path& vad_model_path,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us,
    const std::filesystem::path& aligner_bundle_dir = {});

}  // namespace svp::audio
