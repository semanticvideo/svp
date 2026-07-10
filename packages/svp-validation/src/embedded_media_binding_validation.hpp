#pragma once

#include "svp/package/embedded_svpi.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>

namespace svp::validation {

void add_embedded_media_binding_finding(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const std::filesystem::path& container_path,
    const svp::package::EmbeddedSvpiInfo& embedding);

}  // namespace svp::validation
