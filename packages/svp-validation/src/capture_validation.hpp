#pragma once

#include "svp/package/package_layout.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>

namespace svp::validation {

// RC2 §23.5: provenance/capture.json is optional; when present it validates
// against capture.schema.json.
void add_capture_findings(ValidationReport& report, const ValidationCodeRegistry& registry,
                          const std::filesystem::path& package_path,
                          const svp::package::PackageLayout& layout,
                          const nlohmann::json& capture_schema);

}  // namespace svp::validation
