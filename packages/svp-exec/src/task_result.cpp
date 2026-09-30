#include "svp/exec/task_result.hpp"

#include "artifact_ref_json.hpp"
#include "json_fields.hpp"
#include "record_identifiers.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/output_digest.hpp"
#include "task_execution_json.hpp"

#include <string>

namespace svp::exec {
namespace {

constexpr std::string_view kRoot = "task_result";

TaskStatus parse_status(const std::string& value) {
  if (value == task_status_name(TaskStatus::succeeded)) {
    return TaskStatus::succeeded;
  }
  if (value == task_status_name(TaskStatus::failed)) {
    return TaskStatus::failed;
  }
  throw ExecError(ExecErrorCode::invalid_value,
                  "task_result.status must be \"succeeded\" or \"failed\": `" +
                      value + "`");
}

nlohmann::json error_to_json(const TaskError& error) {
  return nlohmann::json{{"code", error.code},
                        {"message", error.message},
                        {"retryable", error.retryable}};
}

TaskError error_from_json(const nlohmann::json& value, std::string_view path) {
  detail::reject_unknown_fields(value, {"code", "message", "retryable"}, path);
  return TaskError{.code = detail::required_string(value, "code", path),
                   .message = detail::required_string(value, "message", path),
                   .retryable = detail::required_bool(value, "retryable", path)};
}

void validate_status_consistency(const TaskResult& result) {
  if (result.status == TaskStatus::succeeded) {
    if (result.error) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task_result.error must be absent when status is "
                      "succeeded");
    }
    return;
  }
  if (!result.error) {
    throw ExecError(ExecErrorCode::missing_field,
                    "task_result.error is required when status is failed");
  }
  if (!detail::is_lower_identifier(result.error->code)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_result.error.code must be a lowercase identifier: `" +
                        result.error->code + "`");
  }
  if (!result.outputs.empty()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_result.outputs must be empty when status is failed");
  }
}

}  // namespace

std::string_view task_status_name(TaskStatus status) noexcept {
  switch (status) {
    case TaskStatus::succeeded:
      return "succeeded";
    case TaskStatus::failed:
      return "failed";
  }
  return "unknown";
}

void validate_task_result(const TaskResult& result) {
  if (!detail::is_record_identifier(result.task_id)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_result.task_id must be a non-empty [A-Za-z0-9._-] "
                    "identifier: `" +
                        result.task_id + "`");
  }
  if (result.attempt == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_result.attempt must be at least 1");
  }
  if (!result.diagnostics.is_object()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    "task_result.diagnostics must be a JSON object");
  }
  static_cast<void>(encode_canonical_json(result.diagnostics));
  validate_status_consistency(result);
  detail::validate_task_execution(result.execution);
  for (const ArtifactRef& output : result.outputs) {
    validate_artifact_ref(output);
  }
  if (compute_output_digest(result.outputs) != result.output_digest) {
    throw ExecError(ExecErrorCode::digest_mismatch,
                    "task_result.output_digest does not match the outputs");
  }
}

nlohmann::json task_result_to_json(const TaskResult& result) {
  validate_task_result(result);
  nlohmann::json outputs = nlohmann::json::array();
  for (const ArtifactRef& output : result.outputs) {
    outputs.push_back(artifact_ref_to_json(output));
  }
  nlohmann::json value{
      {"attempt", result.attempt},
      {"diagnostics", result.diagnostics},
      {"execution", detail::task_execution_to_json(result.execution)},
      {"output_digest", blake3_prefixed(result.output_digest)},
      {"outputs", std::move(outputs)},
      {"schema", std::string(kTaskResultSchema)},
      {"status", std::string(task_status_name(result.status))},
      {"task_id", result.task_id}};
  if (result.error) {
    value["error"] = error_to_json(*result.error);
  }
  return value;
}

TaskResult task_result_from_json(const nlohmann::json& value) {
  detail::require_object(value, kRoot);
  detail::reject_unknown_fields(
      value,
      {"attempt", "diagnostics", "error", "execution", "output_digest", "outputs",
       "schema", "status", "task_id"},
      kRoot);
  if (detail::required_string(value, "schema", kRoot) != kTaskResultSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_result.schema must be \"" +
                        std::string(kTaskResultSchema) + "\"");
  }

  TaskResult result;
  result.task_id = detail::required_string(value, "task_id", kRoot);
  result.attempt = detail::required_unsigned(value, "attempt", kRoot);
  result.status = parse_status(detail::required_string(value, "status", kRoot));

  const std::string outputs_path = detail::child_path(kRoot, "outputs[]");
  for (const nlohmann::json& output :
       detail::required_array(value, "outputs", kRoot)) {
    result.outputs.push_back(
        detail::artifact_ref_from_json_at(output, outputs_path));
  }
  result.output_digest =
      detail::required_blake3_prefixed(value, "output_digest", kRoot);
  result.execution = detail::task_execution_from_json_at(
      detail::required_field(value, "execution", kRoot),
      detail::child_path(kRoot, "execution"));
  result.diagnostics = detail::required_object(value, "diagnostics", kRoot);
  if (value.contains("error")) {
    result.error = error_from_json(detail::required_object(value, "error", kRoot),
                                   detail::child_path(kRoot, "error"));
  }

  validate_task_result(result);
  return result;
}

std::string encode_task_result(const TaskResult& result) {
  return encode_canonical_json(task_result_to_json(result));
}

TaskResult decode_task_result(std::string_view bytes) {
  return task_result_from_json(decode_canonical_json(bytes));
}

}  // namespace svp::exec
