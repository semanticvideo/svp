#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

/**
 * Generates the first SQLite index foundation (`index.sqlite`) and its manifest
 * (`index_manifest.json`) inside the package staging directory.
 *
 * If staged OCR text or color observations exist, it parses them and populates
 * the SQLite tables. It also generates valid logical row stream digests and file hashes.
 *
 * Returns true if successful.
 */
[[nodiscard]] bool write_index_foundation(
    const std::filesystem::path& staging_dir,
    const nlohmann::json& manifest_json);

}  // namespace svp::package
