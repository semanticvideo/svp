#pragma once

#include "svp/validation/report.hpp"

#include <filesystem>

namespace svp::validation {

struct ValidatorOptions {
  std::filesystem::path validation_codes_path;
};

[[nodiscard]] ValidationReport validate_package(const std::filesystem::path& package_path,
                                                const ValidatorOptions& options);

}  // namespace svp::validation
