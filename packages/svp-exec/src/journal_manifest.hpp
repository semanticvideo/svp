#pragma once

// journal_manifest.json. RC2 §20.4 names the file but not its content; this
// is the reference builder's format. It is written last during create(), so
// a journal without a readable manifest was never fully initialized.

#include "svp/exec/journal_layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace svp::exec::detail {

inline constexpr std::string_view kJournalManifestFormat = "svp-recovery-journal-v1";

struct JournalManifest {
  std::string build_session_id;
  std::string created_utc;
  std::string output_path;
  std::int64_t journal_schema_version = 0;
};

// Canonical JSON, staged in blobs/pending, synced, and renamed into place.
void write_journal_manifest(const JournalLayout& layout, const JournalManifest& manifest);

// Throws JournalError(incompatible) when missing, malformed, or of another
// format or schema version.
[[nodiscard]] JournalManifest read_journal_manifest(const JournalLayout& layout);

}  // namespace svp::exec::detail
