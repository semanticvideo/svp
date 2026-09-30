#pragma once

// Test-only TaskArtifactAccess: output bytes live in memory, keyed by BLAKE3.
// Toy tasks take no inputs, so any declared input is unresolved.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_artifact_access.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace svp::exec::test {

class InMemoryArtifactStore final : public TaskArtifactAccess {
 public:
  ArtifactRef put(std::vector<std::byte> bytes, std::string media_type, std::string role);

  [[nodiscard]] ResolvedInputs resolve_inputs(const TaskSpec& spec) override;
  [[nodiscard]] std::vector<FramePayload> read_outputs(const TaskResult& result) override;

 private:
  std::mutex mutex_;
  std::map<Blake3Digest, std::vector<std::byte>> blobs_;
};

}  // namespace svp::exec::test
