#pragma once

#include "jsonl_line_reader.hpp"

#include <cstdint>
#include <filesystem>

namespace package_export {

// The largest export.json read when deciding whether --overwrite may replace
// a directory. It is the export's one-document memory bound: an export.json
// is one JSON document, and anything larger is not read and so is never
// treated as a previous export (the safe answer for a deletion guard).
inline constexpr std::uint64_t kMaxPreviousSummaryBytes = kMaxRecordBytes;

// True only for a directory this exporter wrote (Package_Export_v1.md
// Section 1): its export.json is a regular file (not a link) of at most
// `max_summary_bytes`, parses as a JSON object whose `export_format` is
// "svp-package-export" and whose `export_format_version` this build
// supports, and it has a real `layers/` subdirectory. Anything else, including
// any read error, is not a previous export.
[[nodiscard]] bool is_previous_export(
    const std::filesystem::path& directory,
    std::uint64_t max_summary_bytes = kMaxPreviousSummaryBytes);

}  // namespace package_export
