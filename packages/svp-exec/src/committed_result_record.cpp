#include "committed_result_record.hpp"

#include "json_fields.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"

namespace svp::exec::detail {
namespace {

constexpr std::string_view kPath = "committed_result";
constexpr std::string_view kExecutorId = "executor_id";
constexpr std::string_view kSchema = "schema";
constexpr std::string_view kTaskResult = "task_result";

}  // namespace

std::string encode_committed_result_record(const TaskResult& result,
                                           std::string_view executor_id) {
  return encode_canonical_json(
      nlohmann::json{{std::string(kExecutorId), std::string(executor_id)},
                     {std::string(kSchema), std::string(kCommittedResultRecordSchema)},
                     {std::string(kTaskResult), task_result_to_json(result)}});
}

CommittedResultRecord decode_committed_result_record(std::string_view bytes) {
  const nlohmann::json record = decode_canonical_json(bytes);
  require_object(record, kPath);
  reject_unknown_fields(record, {kExecutorId, kSchema, kTaskResult}, kPath);
  if (required_string(record, kSchema, kPath) != kCommittedResultRecordSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "committed_result.schema is not " +
                        std::string(kCommittedResultRecordSchema));
  }
  return CommittedResultRecord{
      .result = task_result_from_json(required_object(record, kTaskResult, kPath)),
      .executor_id = required_string(record, kExecutorId, kPath)};
}

}  // namespace svp::exec::detail
