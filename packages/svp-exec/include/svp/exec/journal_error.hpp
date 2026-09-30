#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace svp::exec {

// Journal failures are build failures (RC2 §20.4 makes the journal
// mandatory), so unlike the cache they are thrown.
enum class JournalErrorCode {
  // create(): a journal directory already exists for this output.
  already_exists,
  // open(): no journal directory exists for this output.
  not_found,
  // Another process holds locks/build.lock.
  locked,
  // Manifest or schema missing, unreadable, or from another journal version.
  incompatible,
  io_error,
  database_error,
  // A task, artifact, or attempt key is already recorded.
  duplicate_record,
  // resume(): the source media no longer matches the recorded fingerprint.
  source_mismatch,
  unknown_task,
  invalid_transition,
  invalid_argument,
  // Artifact bytes do not hash to the digest the caller declared.
  artifact_mismatch,
  // The journal was closed or moved from.
  closed,
};

[[nodiscard]] std::string_view journal_error_code_name(JournalErrorCode code) noexcept;

class JournalError : public std::runtime_error {
 public:
  JournalError(JournalErrorCode code, std::string message);

  [[nodiscard]] JournalErrorCode code() const noexcept;

 private:
  JournalErrorCode code_;
};

}  // namespace svp::exec
