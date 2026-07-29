#pragma once

#include <filesystem>

namespace svp::core {

[[nodiscard]] std::filesystem::path resolve_executable(
    const std::filesystem::path& candidate);

[[nodiscard]] std::filesystem::path resolve_current_executable(
    const std::filesystem::path& invoked_path = {});

}  // namespace svp::core
