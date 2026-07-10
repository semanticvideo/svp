#pragma once

#include "svp/validation/report.hpp"
#include "svp/package/embedded_svpi.hpp"

#include <filesystem>
#include <string_view>

namespace svp::validation {

struct EmbeddedSvpiValidatorOptions {
  std::filesystem::path validation_codes_path;
  std::filesystem::path registry_root_path;
  std::filesystem::path schema_root_path;
};

[[nodiscard]] ValidationReport validate_embedded_svpi_mp4(
    const std::filesystem::path& mp4_path,
    const EmbeddedSvpiValidatorOptions& options);

[[nodiscard]] std::string_view validation_code_for_embedded_issue(
    svp::package::EmbeddedSvpiIssueCode code) noexcept;

}  // namespace svp::validation
