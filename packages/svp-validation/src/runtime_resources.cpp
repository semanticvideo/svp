#include "svp/validation/runtime_resources.hpp"

#include "svp/core/executable_path.hpp"

#include "spec_assets.hpp"

#include <array>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace svp::validation {
namespace {

constexpr std::string_view kValidationCodesRelativePath =
    "registries/validation-codes.json";

std::filesystem::path resolved_executable(
    const std::filesystem::path& executable_path) {
  if (executable_path.empty()) {
    try {
      return svp::core::resolve_current_executable();
    } catch (const std::exception& error) {
      throw std::runtime_error(
          "SVP runtime resources could not be located because the running "
          "executable could not be resolved: " +
          std::string(error.what()) +
          "\nCLI callers can use --validation-codes <path>.");
    }
  }

  const auto resolved = svp::core::resolve_executable(executable_path);
  if (resolved.empty()) {
    throw std::runtime_error(
        "SVP runtime resources could not be located because the supplied "
        "executable path could not be resolved: " + executable_path.string() +
        "\nCLI callers can use --validation-codes <path>.");
  }
  return resolved;
}

std::array<std::filesystem::path, 2> candidate_resource_roots(
    const std::filesystem::path& executable_path) {
  const auto executable_directory = executable_path.parent_path();
  return {
      (executable_directory / ".." / "share" / "svp").lexically_normal(),
      (executable_directory / ".." / ".." / "share" / "svp")
          .lexically_normal(),
  };
}

std::filesystem::path resource_path_for(const std::filesystem::path& root,
                                        const SpecAsset& asset) {
  const auto directory = asset.kind == SpecAssetKind::registry
                             ? "registries"
                             : "schemas";
  return root / directory / asset.relative_path;
}

std::vector<std::filesystem::path> missing_resource_paths(
    const std::filesystem::path& root) {
  std::vector<std::filesystem::path> missing;
  const auto require_regular_file = [&](const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
      missing.push_back(path);
    }
  };

  require_regular_file(root / kValidationCodesRelativePath);
  for (const auto& asset : required_rc2_spec_assets()) {
    require_regular_file(resource_path_for(root, asset));
  }
  return missing;
}

std::runtime_error missing_resources_error(
    const std::vector<std::filesystem::path>& candidates) {
  std::ostringstream message;
  message << "SVP runtime resources could not be located.";
  for (const auto& candidate : candidates) {
    message << "\nAttempted resource root: " << candidate.string();
    for (const auto& missing : missing_resource_paths(candidate)) {
      message << "\nMissing required resource: " << missing.string();
    }
  }
  message << "\nCLI callers can use --validation-codes <path>.";
  return std::runtime_error(message.str());
}

}  // namespace

RuntimeResourcePaths resolve_default_runtime_resource_paths(
    const std::filesystem::path& executable_path) {
  RuntimeResourcePaths paths;
  const auto executable = resolved_executable(executable_path);
  for (const auto& candidate : candidate_resource_roots(executable)) {
    paths.attempted_resource_roots.push_back(candidate);
    if (!missing_resource_paths(candidate).empty()) {
      continue;
    }

    paths.validation_codes_path = candidate / kValidationCodesRelativePath;
    paths.registry_root_path = candidate / "registries";
    paths.schema_root_path = candidate / "schemas";
    return paths;
  }

  throw missing_resources_error(paths.attempted_resource_roots);
}

RuntimeResourcePaths resolve_validation_resource_paths(
    const std::filesystem::path& validation_codes_path,
    const std::filesystem::path& registry_root_path,
    const std::filesystem::path& schema_root_path,
    const std::filesystem::path& executable_path) {
  RuntimeResourcePaths paths;
  if (validation_codes_path.empty()) {
    paths = resolve_default_runtime_resource_paths(executable_path);
  } else {
    paths.validation_codes_path = validation_codes_path;
    paths.registry_root_path = validation_codes_path.parent_path();
    paths.schema_root_path =
        paths.registry_root_path.parent_path() / "schemas";
  }

  if (!registry_root_path.empty()) {
    paths.registry_root_path = registry_root_path;
  }
  if (!schema_root_path.empty()) {
    paths.schema_root_path = schema_root_path;
  }
  return paths;
}

}  // namespace svp::validation
