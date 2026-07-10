#pragma once

#include "svp/validation/report.hpp"

#include <filesystem>

namespace svp::validation {

struct SvpiValidatorOptions {
  std::filesystem::path validation_codes_path;
  std::filesystem::path registry_root_path;
  std::filesystem::path schema_root_path;
  bool allow_embedded_transport = false;
};

[[nodiscard]] ValidationReport validate_svpi_package(
    const std::filesystem::path& package_path,
    const SvpiValidatorOptions& options);

}  // namespace svp::validation
