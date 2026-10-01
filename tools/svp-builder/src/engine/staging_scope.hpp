#pragma once

// Which part of the shared staging directory a whole-stage task owns.
//
// Whole-stage tasks run today's stage code, which writes straight into one
// staging directory. Each task type declares the staging paths it writes as a
// scope: relative path prefixes in generic '/' form. "colors/" covers a
// directory tree, "spatial/depth." covers files whose name starts with it, and
// "provenance/processors.jsonl" covers one file.
//
// After a task runs, everything inside its scope (files, and directories, which
// the package writer records even when empty) is its output. Restoring that
// capture into a fresh staging directory reproduces exactly what the task left
// behind, which is how a resumed build skips the task.
//
// Correctness needs two rules, enforced by the planner and the end-of-build
// check (staging_capture_check.hpp):
//   * tasks whose scopes overlap are ordered by a dependency path, so captures
//     never race and restoring in graph order replays their writes in order;
//   * every staging entry a build leaves is inside some task's scope.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::engine {

struct StagingScope {
  std::vector<std::string> prefixes;
};

[[nodiscard]] bool scope_covers(const StagingScope& scope,
                                std::string_view relative_path);
[[nodiscard]] bool scopes_overlap(const StagingScope& left,
                                  const StagingScope& right);

enum class StagedEntryKind { file, directory };

// One captured staging entry. Directory paths end with '/'.
struct StagedEntry {
  std::string relative_path;
  StagedEntryKind kind = StagedEntryKind::file;
  std::vector<std::byte> bytes;  // files only
};

// Every regular file and directory under `staging_dir` (relative, generic
// form, directories with a trailing '/'), sorted by path. Missing staging
// directory: empty.
[[nodiscard]] std::vector<std::string> list_staging_entries(
    const std::filesystem::path& staging_dir);

// The entries inside `scope`, sorted by path, files with their bytes.
[[nodiscard]] std::vector<StagedEntry> capture_staging_scope(
    const std::filesystem::path& staging_dir, const StagingScope& scope);

// Makes `scope` hold exactly `entries`: removes files and empty directories
// inside the scope that are not listed, then creates listed directories and
// writes listed files. Throws std::runtime_error on I/O failure.
void restore_staging_scope(const std::filesystem::path& staging_dir,
                           const StagingScope& scope,
                           const std::vector<StagedEntry>& entries);

}  // namespace svp::builder::engine
