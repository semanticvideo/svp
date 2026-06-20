#pragma once

#include "svp/package/package_layout.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>

struct sqlite3;

namespace svp::validation {

void add_ocr_color_index_consistency_findings(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout,
    sqlite3& database);

}  // namespace svp::validation
