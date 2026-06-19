#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace svp::core {

[[nodiscard]] std::filesystem::path normalize_path(std::filesystem::path path);
[[nodiscard]] bool has_extension(std::filesystem::path path, std::string_view extension);

}  // namespace svp::core

