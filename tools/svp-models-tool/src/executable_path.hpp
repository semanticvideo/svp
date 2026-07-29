#pragma once

#include <filesystem>

namespace svp::models::tool {

std::filesystem::path resolve_current_executable(
    const std::filesystem::path& invoked_path);

}  // namespace svp::models::tool
