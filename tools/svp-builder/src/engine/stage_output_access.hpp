#pragma once

// TaskArtifactAccess for tasks run in this process (the coordinator's own
// executors).
//
//   * Whole-stage tasks take no artifact inputs (they read the shared
//     staging directory and their dependencies' committed states) and hand
//     their encoded outputs here before returning (stage()); the attempt
//     runner reads them back once, right after the task returns, to commit
//     them.
//   * Frame-batch tasks read the build's source, registered here by its
//     ArtifactRef once the build fingerprinted it (register_input()), and
//     store their output bytes by content (put()), read back by digest.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/task_artifact_access.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace svp::builder::engine {

// What happens to an output once the attempt runner has read it back.
enum class StageOutputRetention {
  // Kept for the access's lifetime (whole-stage and OCR frame-batch tasks).
  keep,
  // Released after it has been read as many times as it was put: for the
  // dispatched vision tasks (vision_dispatch_setup.hpp), whose outputs the
  // scheduler hands to their stage right away and nothing reads again (they
  // are not journaled), so the coordinator's memory does not grow with a
  // stage's item count.
  release_after_read,
};

class StageOutputAccess final : public svp::exec::TaskArtifactAccess {
 public:
  explicit StageOutputAccess(StageOutputRetention retention = StageOutputRetention::keep)
      : retention_(retention) {}

  // Holds `payloads` for the task's next read_outputs (replacing any left by
  // an abandoned attempt).
  void stage(const std::string& task_id, std::vector<svp::exec::FramePayload> payloads);

  // Makes `ref` resolvable to `path`, a local file the build already
  // verified hashes to `ref` (the journal's source fingerprint).
  void register_input(const svp::exec::ArtifactRef& ref, std::filesystem::path path);

  // Keeps `bytes` under their digest and returns their ref.
  [[nodiscard]] svp::exec::ArtifactRef put(std::span<const std::byte> bytes,
                                           std::string media_type, std::string role);

  // Throws ExecError(unresolved_input) for an input that was not registered
  // with exactly that digest and length.
  [[nodiscard]] svp::exec::ResolvedInputs resolve_inputs(
      const svp::exec::TaskSpec& spec) override;
  // Staged payloads for the task when there are any, otherwise each output's
  // bytes by digest (released afterwards under release_after_read once every
  // put of those bytes has been read). Throws std::runtime_error when an
  // output is missing or the staged count differs from the result's outputs.
  [[nodiscard]] std::vector<svp::exec::FramePayload> read_outputs(
      const svp::exec::TaskResult& result) override;

  // Distinct outputs held now.
  [[nodiscard]] std::size_t stored_outputs() const;

 private:
  struct StoredOutput {
    svp::exec::FramePayload bytes;
    // Puts not yet read back (release_after_read).
    std::size_t unread = 0;
  };

  StageOutputRetention retention_;
  mutable std::mutex mutex_;
  std::map<std::string, std::vector<svp::exec::FramePayload>> pending_;
  std::map<svp::exec::Blake3Digest, std::pair<std::uint64_t, std::filesystem::path>> inputs_;
  std::map<svp::exec::Blake3Digest, StoredOutput> outputs_;
};

}  // namespace svp::builder::engine
