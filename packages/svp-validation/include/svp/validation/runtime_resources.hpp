#pragma once

#include <filesystem>
#include <vector>

namespace svp::validation {

struct RuntimeResourcePaths {
  std::filesystem::path validation_codes_path;
  std::filesystem::path registry_root_path;
  std::filesystem::path schema_root_path;
  std::vector<std::filesystem::path> attempted_resource_roots;
};

[[nodiscard]] RuntimeResourcePaths resolve_default_runtime_resource_paths(
    const std::filesystem::path& executable_path = {});

[[nodiscard]] RuntimeResourcePaths resolve_validation_resource_paths(
    const std::filesystem::path& validation_codes_path,
    const std::filesystem::path& registry_root_path,
    const std::filesystem::path& schema_root_path,
    const std::filesystem::path& executable_path = {});

}  // namespace svp::validation
