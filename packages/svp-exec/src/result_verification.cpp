#include "result_verification.hpp"

#include "svp/exec/exec_error.hpp"

namespace svp::exec::detail {

std::optional<std::string> find_result_defect(const TaskSpec& spec, std::uint64_t attempt,
                                              const AttemptOutput& output) {
  return find_result_defect(spec, attempt, output.result, output.payloads);
}

std::optional<std::string> find_result_defect(const TaskSpec& spec, std::uint64_t attempt,
                                              const TaskResult& result,
                                              std::span<const FramePayload> payloads) {
  try {
    validate_task_result(result);
  } catch (const ExecError& error) {
    return std::string("result record rejected: ") + error.what();
  }
  if (result.task_id != spec.task_id) {
    return "result names task `" + result.task_id + "`, expected `" + spec.task_id + "`";
  }
  if (result.attempt != attempt) {
    return "result names attempt " + std::to_string(result.attempt) + ", expected " +
           std::to_string(attempt);
  }
  if (payloads.size() != result.outputs.size()) {
    return "result carries " + std::to_string(payloads.size()) +
           " payloads for " + std::to_string(result.outputs.size()) + " outputs";
  }
  for (std::size_t index = 0; index < result.outputs.size(); ++index) {
    const ArtifactRef& ref = result.outputs[index];
    if (payloads[index].size() != ref.bytes || blake3_digest(payloads[index]) != ref.blake3) {
      return "payload " + std::to_string(index) + " does not match outputs[" +
             std::to_string(index) + "]";
    }
  }
  return std::nullopt;
}

}  // namespace svp::exec::detail
