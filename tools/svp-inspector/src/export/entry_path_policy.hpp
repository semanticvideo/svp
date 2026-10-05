#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace package_export {

// Package entry names become file paths under the export's layers/
// directory, so every name is checked before anything is written.
//
// A safe name is a relative, '/'-separated path with no empty, "." or ".."
// segment, no backslash, and no control character. Directory entries may end
// in one '/'. Throws ExportError(unsafe_entry_name) otherwise.
void require_safe_entry_name(std::string_view name, bool is_directory);

// Every segment of a planned output path must fit the platform's NAME_MAX.
// Throws ExportError(unsafe_entry_name) naming `entry` otherwise.
void require_output_segments_fit(std::string_view output_path,
                                 std::string_view entry);

struct PlannedOutputPath {
  std::string path;
  std::string entry;
};

// Refuses two planned files that would be the same file on a
// case-insensitive file system, and a planned file whose path is also needed
// as a directory by another planned file. Throws
// ExportError(output_path_collision).
void require_no_output_collisions(const std::vector<PlannedOutputPath>& files);

}  // namespace package_export
