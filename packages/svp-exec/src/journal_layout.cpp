#include "svp/exec/journal_layout.hpp"

namespace svp::exec {
namespace {

// Directory of committed blobs relative to the journal root. Written into
// `artifact.relative_path` with '/' separators on every platform.
constexpr std::string_view kCompletedRelativeDir = "blobs/completed/";

}  // namespace

JournalLayout journal_layout_for(const std::filesystem::path& output_path) {
  std::filesystem::path root = output_path;
  root += kJournalDirectorySuffix;
  return JournalLayout{.root = root,
                       .database = root / "build.sqlite",
                       .manifest = root / "journal_manifest.json",
                       .pending = root / "blobs" / "pending",
                       .completed = root / "blobs" / "completed",
                       .locks = root / "locks",
                       .lock_file = root / "locks" / "build.lock"};
}

std::string journal_completed_relative_path(std::string_view blake3_hex) {
  return std::string(kCompletedRelativeDir) + std::string(blake3_hex);
}

}  // namespace svp::exec
