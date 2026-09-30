#pragma once

// The journal's copy of one committed TaskResult (see
// journal_result_commit_sink.hpp). Canonical JSON, stored as the task's
// `<task_id>.result` artifact:
//   {"executor_id":"<id>","schema":"svp-journal-committed-result-v1",
//    "task_result":{<TaskResult JSON>}}
// The TaskResult keeps what the journal's own rows cannot (output media types,
// roles, and order; the committed attempt and execution identity), so resume
// can hand reducers exactly the result the interrupted run committed.

#include "svp/exec/task_result.hpp"

#include <string>
#include <string_view>

namespace svp::exec::detail {

inline constexpr std::string_view kCommittedResultRecordSchema =
    "svp-journal-committed-result-v1";

struct CommittedResultRecord {
  TaskResult result;
  std::string executor_id;
};

[[nodiscard]] std::string encode_committed_result_record(const TaskResult& result,
                                                         std::string_view executor_id);

// Throws ExecError for non-canonical bytes, a wrong schema, unknown fields, or
// an invalid TaskResult.
[[nodiscard]] CommittedResultRecord decode_committed_result_record(std::string_view bytes);

}  // namespace svp::exec::detail
