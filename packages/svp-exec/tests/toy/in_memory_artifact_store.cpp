#include "in_memory_artifact_store.hpp"

#include "svp/exec/exec_error.hpp"

#include <utility>

namespace svp::exec::test {

ArtifactRef InMemoryArtifactStore::put(std::vector<std::byte> bytes, std::string media_type,
                                       std::string role) {
  ArtifactRef ref = make_artifact_ref(bytes, std::move(media_type), std::move(role));
  const std::lock_guard lock(mutex_);
  blobs_.emplace(ref.blake3, std::move(bytes));
  return ref;
}

ResolvedInputs InMemoryArtifactStore::resolve_inputs(const TaskSpec& spec) {
  if (!spec.inputs.empty()) {
    throw ExecError(ExecErrorCode::unresolved_input,
                    "the in-memory test store resolves no inputs");
  }
  return {};
}

std::vector<FramePayload> InMemoryArtifactStore::read_outputs(const TaskResult& result) {
  const std::lock_guard lock(mutex_);
  std::vector<FramePayload> payloads;
  for (const ArtifactRef& output : result.outputs) {
    const auto found = blobs_.find(output.blake3);
    if (found == blobs_.end()) {
      throw ExecError(ExecErrorCode::unresolved_input,
                      "output " + blake3_hex(output.blake3) + " is not in the store");
    }
    payloads.push_back(found->second);
  }
  return payloads;
}

}  // namespace svp::exec::test
