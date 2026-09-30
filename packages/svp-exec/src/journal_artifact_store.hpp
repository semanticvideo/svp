#pragma once

// Moves task outputs into <journal>/blobs/completed/<64 hex> through the
// shared pending -> fsync -> verify -> rename path (RC2 §20.4).

#include "svp/exec/journal_layout.hpp"
#include "svp/exec/journal_records.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace svp::exec::detail {

struct CommittedBlob {
  std::string relative_path;
  std::uint64_t bytes = 0;
};

// Stages and verifies every artifact before publishing any, so a mismatch
// (JournalError artifact_mismatch) or I/O failure (io_error) leaves no new
// completed blob. A failure while publishing can leave completed blobs no
// row references; resume() removes those.
[[nodiscard]] std::vector<CommittedBlob> store_task_artifacts(
    const JournalLayout& layout, std::span<const JournalArtifactInput> artifacts);

}  // namespace svp::exec::detail
