#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

/**
 * Writes the validation report JSON to the RC2-correct package path
 * `provenance/validation.json` inside the staging directory.
 *
 * Per spec Section 18.2, a completed reference-builder package MUST
 * include `provenance/validation.json`. This function stores the
 * machine-readable validation report emitted by the validator so
 * that a subsequent package re-write includes it in the archive.
 *
 * Returns true if the file was written successfully.
 */
[[nodiscard]] bool write_validation_report_to_staging(
    const std::filesystem::path& staging_dir,
    const nlohmann::json& validation_report);

}  // namespace svp::package
