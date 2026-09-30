#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/task_execution.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

inline constexpr std::string_view kTaskResultSchema = "svp-task-result-v1";

enum class TaskStatus {
  succeeded,
  failed,
};

[[nodiscard]] std::string_view task_status_name(TaskStatus status) noexcept;

// Why an attempt failed. `retryable` separates the plan's failed(retryable)
// from failed(permanent) (§4.4). JSON: {"code","message","retryable"}.
struct TaskError {
  std::string code;
  std::string message;
  bool retryable = false;

  bool operator==(const TaskError&) const = default;
};

// Outcome of one attempt of one task (plan §4.2). Canonical JSON form:
//   {"attempt", "diagnostics":{...}, ["error":{...},] "execution":{...},
//    "output_digest":"b3:<hex>", "outputs":[ArtifactRef...],
//    "schema":"svp-task-result-v1", "status":"succeeded"|"failed",
//    "task_id"}
// `error` is present exactly when status is failed; failed results carry no
// outputs. Unknown fields are rejected.
struct TaskResult {
  std::string task_id;
  std::uint64_t attempt = 0;
  TaskStatus status = TaskStatus::succeeded;
  std::vector<ArtifactRef> outputs;
  Blake3Digest output_digest{};
  TaskExecution execution;
  nlohmann::json diagnostics = nlohmann::json::object();
  std::optional<TaskError> error;

  bool operator==(const TaskResult&) const = default;
};

// Semantic validation shared by encode and decode: identifiers, attempt >= 1,
// status/error/outputs consistency, finite diagnostics, and
// output_digest == compute_output_digest(outputs)
// (ExecError(digest_mismatch) when it differs).
void validate_task_result(const TaskResult& result);

[[nodiscard]] nlohmann::json task_result_to_json(const TaskResult& result);
[[nodiscard]] TaskResult task_result_from_json(const nlohmann::json& value);

[[nodiscard]] std::string encode_task_result(const TaskResult& result);
[[nodiscard]] TaskResult decode_task_result(std::string_view bytes);

}  // namespace svp::exec
