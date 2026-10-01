#include "engine/stage_output_access.hpp"

#include "svp/exec/exec_error.hpp"

#include <stdexcept>

namespace svp::builder::engine {

void StageOutputAccess::stage(const std::string& task_id,
                              std::vector<svp::exec::FramePayload> payloads) {
  const std::lock_guard lock(mutex_);
  pending_.insert_or_assign(task_id, std::move(payloads));
}

void StageOutputAccess::register_input(const svp::exec::ArtifactRef& ref,
                                       std::filesystem::path path) {
  const std::lock_guard lock(mutex_);
  inputs_.insert_or_assign(ref.blake3, std::make_pair(ref.bytes, std::move(path)));
}

svp::exec::ArtifactRef StageOutputAccess::put(std::span<const std::byte> bytes,
                                              std::string media_type, std::string role) {
  svp::exec::ArtifactRef ref =
      svp::exec::make_artifact_ref(bytes, std::move(media_type), std::move(role));
  const std::lock_guard lock(mutex_);
  outputs_.try_emplace(ref.blake3, bytes.begin(), bytes.end());
  return ref;
}

svp::exec::ResolvedInputs StageOutputAccess::resolve_inputs(
    const svp::exec::TaskSpec& spec) {
  svp::exec::ResolvedInputs resolved;
  const std::lock_guard lock(mutex_);
  for (const auto& [name, ref] : spec.inputs) {
    const auto found = inputs_.find(ref.blake3);
    if (found == inputs_.end() || found->second.first != ref.bytes) {
      throw svp::exec::ExecError(svp::exec::ExecErrorCode::unresolved_input,
                                 "task `" + spec.task_id + "` input `" + name +
                                     "` is not a file this build registered");
    }
    resolved.emplace(name, svp::exec::ResolvedInput{.ref = ref, .path = found->second.second});
  }
  return resolved;
}

std::vector<svp::exec::FramePayload> StageOutputAccess::read_outputs(
    const svp::exec::TaskResult& result) {
  const std::lock_guard lock(mutex_);
  const auto found = pending_.find(result.task_id);
  if (found != pending_.end()) {
    if (found->second.size() != result.outputs.size()) {
      throw std::runtime_error("staged outputs of task `" + result.task_id +
                               "` do not match its result");
    }
    std::vector<svp::exec::FramePayload> payloads = std::move(found->second);
    pending_.erase(found);
    return payloads;
  }
  std::vector<svp::exec::FramePayload> payloads;
  payloads.reserve(result.outputs.size());
  for (const svp::exec::ArtifactRef& ref : result.outputs) {
    const auto stored = outputs_.find(ref.blake3);
    if (stored == outputs_.end()) {
      throw std::runtime_error("no outputs for task `" + result.task_id + "`");
    }
    payloads.push_back(stored->second);
  }
  return payloads;
}

}  // namespace svp::builder::engine
