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

class StageOutputAccess final : public svp::exec::TaskArtifactAccess {
 public:
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
  // bytes by digest. Throws std::runtime_error when an output is missing or
  // the staged count differs from the result's outputs.
  [[nodiscard]] std::vector<svp::exec::FramePayload> read_outputs(
      const svp::exec::TaskResult& result) override;

 private:
  std::mutex mutex_;
  std::map<std::string, std::vector<svp::exec::FramePayload>> pending_;
  std::map<svp::exec::Blake3Digest, std::pair<std::uint64_t, std::filesystem::path>> inputs_;
  std::map<svp::exec::Blake3Digest, svp::exec::FramePayload> outputs_;
};

}  // namespace svp::builder::engine
