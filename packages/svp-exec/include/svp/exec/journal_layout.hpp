#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace svp::exec {

// RC2 §20.4: for an output path `video.svp` the journal is `video.svp-journal/`.
inline constexpr std::string_view kJournalDirectorySuffix = "-journal";

// RC2 §20.4 journal layout:
//   <output>-journal/
//     build.sqlite
//     journal_manifest.json
//     blobs/pending/
//     blobs/completed/
//     locks/build.lock
struct JournalLayout {
  std::filesystem::path root;
  std::filesystem::path database;
  std::filesystem::path manifest;
  std::filesystem::path pending;
  std::filesystem::path completed;
  std::filesystem::path locks;
  std::filesystem::path lock_file;
};

[[nodiscard]] JournalLayout journal_layout_for(const std::filesystem::path& output_path);

// Journal-relative path recorded in `artifact.relative_path` for a completed
// blob: "blobs/completed/<64 hex>".
[[nodiscard]] std::string journal_completed_relative_path(std::string_view blake3_hex);

}  // namespace svp::exec
