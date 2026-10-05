#pragma once

#include "package_kind.hpp"

#include "svp/validation/report.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace package_export {

// Runs the same validation library call `svp-validator validate` makes for
// the detected input form, with the default runtime resources.
[[nodiscard]] svp::validation::ValidationReport validate_for_export(
    const std::filesystem::path& path, PackageKind kind);

// True when `svp-validator validate` would exit 0 for this report.
[[nodiscard]] bool validation_passed(
    const svp::validation::ValidationReport& report) noexcept;

// True when the validator could not load its own registries or schemas, so
// the report says nothing about the package.
[[nodiscard]] bool validation_resources_unavailable(
    const svp::validation::ValidationReport& report) noexcept;

// True when an Embedded SVPI Transport envelope declares a profile version
// this build does not implement.
[[nodiscard]] bool embedded_profile_unsupported(
    const svp::validation::ValidationReport& report) noexcept;

// Path-free, deterministic summary recorded in export.json.
[[nodiscard]] nlohmann::json validation_summary_json(
    const svp::validation::ValidationReport& report);

}  // namespace package_export
