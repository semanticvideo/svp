#pragma once

#include "svp/exec/task_state.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// A task as resume() reconstructed it. Non-committed tasks come back `ready`
// when every dependency is committed and `planned` otherwise, whatever
// in-flight state the interrupted build left behind.
struct ResumedTask {
  std::string task_id;
  std::string task_type;
  TaskState state = TaskState::planned;
  std::vector<std::string> depends_on;
};

enum class DemotionReason {
  artifact_missing,
  artifact_not_regular_file,
  artifact_size_mismatch,
  artifact_hash_mismatch,
  // relative_path or blake3 column is not a well-formed completed-blob path.
  artifact_record_invalid,
  // committed without output_blake3.
  output_digest_missing,
};

[[nodiscard]] std::string_view demotion_reason_name(DemotionReason reason) noexcept;

// A committed task whose recorded outputs failed verification. Its artifact,
// provenance, and cache_hit rows were deleted and it will run again.
struct DemotedTask {
  std::string task_id;
  std::string artifact_id;  // empty for output_digest_missing
  DemotionReason reason = DemotionReason::artifact_missing;
};

struct ResumeReport {
  // Sorted by task_id.
  std::vector<ResumedTask> tasks;
  std::vector<DemotedTask> demoted;
  std::uint64_t discarded_pending_blobs = 0;
  // Files in blobs/completed/ no artifact row references (a crash between
  // rename and commit) or whose content failed verification.
  std::uint64_t discarded_completed_blobs = 0;
  // task_attempt rows left leased/running, now `abandoned`.
  std::uint64_t abandoned_attempts = 0;
};

}  // namespace svp::exec
