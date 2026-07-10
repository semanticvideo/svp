#pragma once

#include "svp/validation/report.hpp"
#include "svp/package/embedded_svpi.hpp"

#include <filesystem>
#include <string_view>

namespace svp::validation {

struct EmbeddedSvpiTransportValidatorOptions {
  std::filesystem::path validation_codes_path;
  std::filesystem::path registry_root_path;
  std::filesystem::path schema_root_path;
};

[[nodiscard]] ValidationReport validate_embedded_svpi_transport(
    const std::filesystem::path& container_path,
    const EmbeddedSvpiTransportValidatorOptions& options);

[[nodiscard]] std::string_view validation_code_for_embedded_issue(
    svp::package::EmbeddedSvpiIssueCode code) noexcept;

}  // namespace svp::validation
