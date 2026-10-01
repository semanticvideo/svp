#include "engine/stage_output_access.hpp"

#include "svp/exec/exec_error.hpp"

#include <stdexcept>

namespace svp::builder::engine {

void StageOutputAccess::stage(const std::string& task_id,
                              std::vector<svp::exec::FramePayload> payloads) {
  const std::lock_guard lock(mutex_);
  pending_.insert_or_assign(task_id, std::move(payloads));
}

svp::exec::ResolvedInputs StageOutputAccess::resolve_inputs(
    const svp::exec::TaskSpec& spec) {
  if (!spec.inputs.empty()) {
    throw svp::exec::ExecError(svp::exec::ExecErrorCode::unresolved_input,
                               "whole-stage task `" + spec.task_id +
                                   "` declares artifact inputs");
  }
  return {};
}

std::vector<svp::exec::FramePayload> StageOutputAccess::read_outputs(
    const svp::exec::TaskResult& result) {
  const std::lock_guard lock(mutex_);
  const auto found = pending_.find(result.task_id);
  if (found == pending_.end() || found->second.size() != result.outputs.size()) {
    throw std::runtime_error("no staged outputs for task `" + result.task_id + "`");
  }
  std::vector<svp::exec::FramePayload> payloads = std::move(found->second);
  pending_.erase(found);
  return payloads;
}

}  // namespace svp::builder::engine
