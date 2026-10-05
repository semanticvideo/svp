#pragma once

#include "package_reader.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace package_export {

// Export directory layout (Package_Export_v1.md Section 2).
inline constexpr std::string_view kSummaryFileName = "export.json";
inline constexpr std::string_view kLayersDirectoryName = "layers";

// Files a block stream entry is exported as (Section 6).
inline constexpr std::string_view kBlockTableSuffix = ".blocks.jsonl";
inline constexpr std::string_view kDecodedPayloadSuffix = ".decoded.bin";

enum class Representation {
  jsonl,
  json,
  block_stream,
  file,
};

enum class OutputRole {
  records,
  document,
  bytes,
  block_table,
  decoded_payloads,
};

[[nodiscard]] std::string_view to_string(Representation representation) noexcept;
[[nodiscard]] std::string_view to_string(OutputRole role) noexcept;

// Representation of a package entry, chosen from the entry-name suffixes the
// SVP and SVPI layouts define (Section 4).
[[nodiscard]] Representation representation_for(std::string_view entry_name);

// "layers/<entry>": where an entry mirrored byte for byte (or as records)
// lands, relative to the export root.
[[nodiscard]] std::string layer_file_path(std::string_view entry_name);

struct PlannedFile {
  std::string path;  // relative to the export root
  OutputRole role = OutputRole::bytes;
};

struct PlannedLayer {
  PackageEntry entry;
  Representation representation = Representation::file;
  std::optional<std::string> section;
  std::vector<PlannedFile> files;
};

struct ExportPlan {
  // File entries only, sorted by entry name.
  std::vector<PlannedLayer> layers;
  // Sum of the declared uncompressed entry sizes: a lower bound of what the
  // export writes, used for the free-space check.
  std::uint64_t declared_input_bytes = 0;

  [[nodiscard]] const PlannedLayer* find(std::string_view entry_name) const;
};

// Checks every entry name and every planned output path, then lays out the
// export. Throws ExportError for unsafe names and output collisions.
[[nodiscard]] ExportPlan make_export_plan(
    const std::vector<PackageEntry>& entries);

}  // namespace package_export
