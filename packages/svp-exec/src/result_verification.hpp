#pragma once

#include "svp/exec/executor.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace svp::exec::detail {

// Plan §4.4 "validation of results", applied to every result before commit
// whatever executor produced it: the TaskResult schema and output_digest
// (validate_task_result), that it answers this task and attempt, and that
// payload i has the length and BLAKE3 of outputs[i]. Returns a description of
// the first defect, or nullopt when the result may be committed.
//
// Not yet checked: per-task-type unit counts and runtime_id (plan §4.4); both
// need the task type's result contract and the coordinator's runtime identity.
[[nodiscard]] std::optional<std::string> find_result_defect(const TaskSpec& spec,
                                                            std::uint64_t attempt,
                                                            const AttemptOutput& output);

}  // namespace svp::exec::detail
