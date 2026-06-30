#pragma once

#include "svp/package/media_binding.hpp"
#include "svp/package/package_writer.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct SvpiWriterOptions {
  bool deterministic_ordering = true;
};

/**
 * Assembles a .svpi sidecar package.
 *
 * Steps:
 * 1. Writes to a temporary file (e.g. package_path.tmp).
 * 2. Adds the mimetype file as the first uncompressed entry
 *    with value "application/vnd.svp.interlace+zip".
 * 3. Adds manifest.json with format: "svpi".
 * 4. Adds media_binding.json.
 * 5. Adds required top-level directories (same as SVP minus media/original).
 * 6. Copies staged files from the staging directory.
 * 7. Does NOT add media/original/ primary media bytes.
 * 8. Closes and renames the temporary file atomically to package_path.
 *
 * If any step fails, the temporary file is deleted and no partial output is left.
 */
[[nodiscard]] bool write_svpi_package(
    const std::filesystem::path& package_path,
    const std::filesystem::path& staging_dir,
    const nlohmann::json& manifest_json,
    const MediaBindingDocument& media_binding,
    const SvpiWriterOptions& options = {});

}  // namespace svp::package
