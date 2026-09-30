#pragma once

#include <filesystem>

namespace svp::exec {

// RC2 §20.5.1: the journal is deleted after successful finalization and
// validation unless diagnostic retention was requested.
enum class JournalRetention { delete_on_success, retain_for_diagnostics };

struct JournalFinish {
  // True only for retain_for_diagnostics. The caller must then report
  // WARN_JOURNAL_RETAINED_DIAGNOSTIC (RC2 §20.5.1 rule 2).
  bool retained = false;
  std::filesystem::path journal_root;
};

// Deletes the journal of `output_path` when no build holds it. Throws
// JournalError(locked) when one does; a missing journal is not an error.
void remove_journal(const std::filesystem::path& output_path);

}  // namespace svp::exec
