#include "journal_artifact_store.hpp"

#include "svp/exec/journal_error.hpp"
#include "verified_blob.hpp"

#include <type_traits>

namespace svp::exec::detail {
namespace {

namespace fs = std::filesystem;

StageResult stage(const JournalLayout& layout, const JournalArtifactInput& artifact) {
  return std::visit(
      [&](const auto& content) {
        using Content = std::decay_t<decltype(content)>;
        if constexpr (std::is_same_v<Content, fs::path>) {
          return stage_blob_file(layout.pending, content, artifact.blake3);
        } else {
          return stage_blob_bytes(layout.pending, content, artifact.blake3);
        }
      },
      artifact.content);
}

void discard_all(const std::vector<StagedBlob>& staged) noexcept {
  for (const StagedBlob& blob : staged) {
    discard_staged_blob(blob);
  }
}

}  // namespace

std::vector<CommittedBlob> store_task_artifacts(const JournalLayout& layout,
                                                std::span<const JournalArtifactInput> artifacts) {
  std::vector<StagedBlob> staged;
  staged.reserve(artifacts.size());
  for (const JournalArtifactInput& artifact : artifacts) {
    const StageResult result = stage(layout, artifact);
    if (result.status == StageStatus::staged) {
      staged.push_back(result.blob);
      continue;
    }
    discard_all(staged);
    if (result.status == StageStatus::digest_mismatch) {
      throw JournalError(JournalErrorCode::artifact_mismatch,
                         "artifact " + artifact.artifact_id + " does not hash to its declared blake3 " +
                             blake3_hex(artifact.blake3));
    }
    throw JournalError(JournalErrorCode::io_error,
                       "stage artifact " + artifact.artifact_id + ": " + result.error.message());
  }

  std::vector<CommittedBlob> committed;
  committed.reserve(staged.size());
  for (std::size_t index = 0; index < staged.size(); ++index) {
    const StagedBlob& blob = staged[index];
    const std::string hex = blake3_hex(blob.digest);
    const fs::path final_path = layout.completed / hex;
    std::error_code error;
    // Identical content already committed by another task is shared.
    if (check_blob(final_path, blob.digest, blob.bytes, error) == BlobCheck::valid) {
      discard_staged_blob(blob);
    } else if ((error = publish_staged_blob(blob, final_path))) {
      for (std::size_t rest = index; rest < staged.size(); ++rest) {
        discard_staged_blob(staged[rest]);
      }
      throw JournalError(JournalErrorCode::io_error,
                         "commit artifact blob " + hex + ": " + error.message());
    }
    committed.push_back(
        CommittedBlob{.relative_path = journal_completed_relative_path(hex), .bytes = blob.bytes});
  }
  return committed;
}

}  // namespace svp::exec::detail
