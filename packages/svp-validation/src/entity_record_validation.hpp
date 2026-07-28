#pragma once

#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>

namespace svp::package {
struct PackageLayout;
}

namespace svp::validation {

void add_entity_record_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& package_path,
                                const svp::package::PackageLayout& layout);

}  // namespace svp::validation
