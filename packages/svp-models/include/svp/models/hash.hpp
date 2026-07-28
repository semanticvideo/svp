#pragma once

#include <filesystem>
#include <string>

namespace svp::models {

[[nodiscard]] std::string blake3_hex_for_file(const std::filesystem::path& path);

[[nodiscard]] std::string blake3_hex_for_model_bundle(
    const std::filesystem::path& bundle_root);

}  // namespace svp::models
