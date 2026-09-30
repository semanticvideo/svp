#include "task_execution_json.hpp"

#include "json_fields.hpp"
#include "record_identifiers.hpp"
#include "svp/exec/exec_error.hpp"

#include <string>

namespace svp::exec::detail {

nlohmann::json task_execution_to_json(const TaskExecution& execution) {
  validate_task_execution(execution);
  return nlohmann::json{
      {"cpu_ms",
       {{"system", execution.cpu_ms.system}, {"user", execution.cpu_ms.user}}},
      {"peak_rss_bytes", execution.peak_rss_bytes},
      {"runtime_id", blake3_prefixed(execution.runtime_id)},
      {"timing_ms",
       {{"compute", execution.timing_ms.compute},
        {"decode", execution.timing_ms.decode},
        {"encode", execution.timing_ms.encode},
        {"input_fetch", execution.timing_ms.input_fetch},
        {"queue", execution.timing_ms.queue}}},
      {"worker_session_id", execution.worker_session_id}};
}

void validate_task_execution(const TaskExecution& execution) {
  if (!is_record_identifier(execution.worker_session_id)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_result.execution.worker_session_id must be a "
                    "non-empty [A-Za-z0-9._-] identifier");
  }
}

TaskExecution task_execution_from_json_at(const nlohmann::json& value,
                                          std::string_view path) {
  require_object(value, path);
  reject_unknown_fields(value,
                        {"cpu_ms", "peak_rss_bytes", "runtime_id", "timing_ms",
                         "worker_session_id"},
                        path);

  const std::string timing_path = child_path(path, "timing_ms");
  const nlohmann::json& timing = required_object(value, "timing_ms", path);
  reject_unknown_fields(
      timing, {"compute", "decode", "encode", "input_fetch", "queue"},
      timing_path);

  const std::string cpu_path = child_path(path, "cpu_ms");
  const nlohmann::json& cpu = required_object(value, "cpu_ms", path);
  reject_unknown_fields(cpu, {"system", "user"}, cpu_path);

  TaskExecution execution{
      .worker_session_id = required_string(value, "worker_session_id", path),
      .runtime_id = required_blake3_prefixed(value, "runtime_id", path),
      .timing_ms =
          TaskTimingMs{
              .queue = required_unsigned(timing, "queue", timing_path),
              .input_fetch = required_unsigned(timing, "input_fetch", timing_path),
              .decode = required_unsigned(timing, "decode", timing_path),
              .compute = required_unsigned(timing, "compute", timing_path),
              .encode = required_unsigned(timing, "encode", timing_path)},
      .cpu_ms = TaskCpuMs{.user = required_unsigned(cpu, "user", cpu_path),
                          .system = required_unsigned(cpu, "system", cpu_path)},
      .peak_rss_bytes = required_unsigned(value, "peak_rss_bytes", path)};
  validate_task_execution(execution);
  return execution;
}

}  // namespace svp::exec::detail
