#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct PackageWriterOptions {
  bool deterministic_ordering = true;
};

/**
 * Assembles a skeleton .svp package.
 *
 * Steps:
 * 1. Writes to a temporary file (e.g. package_path.tmp).
 * 2. Adds the mimetype file as the first uncompressed entry.
 * 3. Adds manifest.json.
 * 4. Adds required top-level directories.
 * 5. Copies the source media file if present (as media/original/source_000.<ext>).
 * 6. Copies staged files from the staging directory.
 * 7. Closes and renames the temporary file atomically to package_path.
 *
 * If any step fails, the temporary file is deleted and no partial output is left.
 */
[[nodiscard]] bool write_package_skeleton(
    const std::filesystem::path& package_path,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& source_path,
    const nlohmann::json& manifest_json,
    const PackageWriterOptions& options = {});

}  // namespace svp::package
