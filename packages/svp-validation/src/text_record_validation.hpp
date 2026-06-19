#pragma once

#include "ocr_color_spec.hpp"

#include "svp/package/package_layout.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>

namespace svp::validation {

void add_text_record_findings(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const std::filesystem::path& package_path,
                              const svp::package::PackageLayout& layout,
                              const OcrColorSpec& spec);

}  // namespace svp::validation
