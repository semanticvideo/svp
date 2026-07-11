#pragma once

#include <filesystem>
#include <optional>

namespace svp::audio {

[[nodiscard]] std::optional<std::filesystem::path> find_whisper_ggml_model(
    const std::filesystem::path& model_dir);

}  // namespace svp::audio
