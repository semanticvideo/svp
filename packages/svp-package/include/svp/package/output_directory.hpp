#pragma once

#include <filesystem>

namespace svp::package {

// Creates the directory that will contain `output_path`, if the path names one.
//
// A bare filename ("clip.svp") has an empty parent_path() and lives in the
// current working directory, which already exists; it must behave exactly like
// "./clip.svp". std::filesystem::create_directories("") fails, so every writer
// of a caller-supplied output path goes through this helper instead of calling
// create_directories(output_path.parent_path()) directly.
// Throws std::filesystem::filesystem_error when a non-empty parent cannot be
// created.
void ensure_parent_directory(const std::filesystem::path& output_path);

}  // namespace svp::package
