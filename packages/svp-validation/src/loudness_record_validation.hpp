#pragma once

#include "svp/package/package_layout.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>

namespace svp::validation {

void add_loudness_record_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& package_path,
                                const svp::package::PackageLayout& layout,
                                const nlohmann::json& loudness_observation_schema,
                                const nlohmann::json& loudness_summary_schema);

}  // namespace svp::validation
