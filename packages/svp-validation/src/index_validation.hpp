#pragma once

#include "svp/package/package_layout.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>

namespace svp::validation {

void add_index_findings(ValidationReport& report,
                        const ValidationCodeRegistry& registry,
                        const std::filesystem::path& package_path,
                        const svp::package::PackageLayout& layout,
                        const std::filesystem::path& schema_root);

}  // namespace svp::validation
