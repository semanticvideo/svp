#include "svp/exec/task_attempt_runner.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/output_digest.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace svp::exec {
namespace {

bool is_retryable_exec_error(ExecErrorCode code) noexcept {
  return code == ExecErrorCode::unresolved_input || code == ExecErrorCode::cancelled;
}

std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

TaskExecution execution_identity(const AttemptContext& context) {
  TaskExecution execution;
  execution.worker_session_id = context.worker_session_id;
  execution.runtime_id = context.runtime_id;
  return execution;
}

AttemptOutput failed_attempt(const TaskSpec& spec, const AttemptContext& context,
                             std::string code, std::string message,
                             bool retryable) {
  TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = context.attempt;
  result.status = TaskStatus::failed;
  result.output_digest = compute_output_digest({});
  result.execution = execution_identity(context);
  result.error = TaskError{
      .code = std::move(code), .message = std::move(message), .retryable = retryable};
  return AttemptOutput{.result = std::move(result), .payloads = {}};
}

}  // namespace

AttemptOutput run_task_attempt(const TaskTypeRegistry& registry,
                               TaskArtifactAccess& artifacts, const TaskSpec& spec,
                               const AttemptContext& context,
                               const CancellationToken& cancellation) {
  try {
    throw_if_cancelled(cancellation, "attempt start");
    const ResolvedInputs inputs = artifacts.resolve_inputs(spec);
    throw_if_cancelled(cancellation, "inputs resolved");
    const auto started = std::chrono::steady_clock::now();
    TaskResult result = registry.execute(spec, inputs, cancellation);
    const std::uint64_t compute_ms = elapsed_ms(started);

    result.attempt = context.attempt;
    result.execution.worker_session_id = context.worker_session_id;
    result.execution.runtime_id = context.runtime_id;
    result.execution.timing_ms.compute = compute_ms;
    validate_task_result(result);

    std::vector<FramePayload> payloads;
    if (result.status == TaskStatus::succeeded) {
      payloads = artifacts.read_outputs(result);
    }
    return AttemptOutput{.result = std::move(result), .payloads = std::move(payloads)};
  } catch (const ExecError& error) {
    return failed_attempt(spec, context, std::string(exec_error_code_name(error.code())),
                          error.what(), is_retryable_exec_error(error.code()));
  } catch (const std::exception& error) {
    return failed_attempt(spec, context, std::string(kTaskExceptionErrorCode),
                          error.what(), true);
  }
}

}  // namespace svp::exec
