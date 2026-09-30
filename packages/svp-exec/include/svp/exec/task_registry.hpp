#pragma once

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/resolved_inputs.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec {

// Returns nullopt when `parameters` satisfy the task type's schema, otherwise
// a human-readable reason.
using TaskParameterValidator =
    std::function<std::optional<std::string>(const nlohmann::json& parameters)>;

// Runs one attempt of a task. `cancellation` is the attempt's own token: the
// executor sets it when the scheduler cancels the lease (build cancelled,
// lease lost, or the attempt ran past its hard deadline, see
// lease_policy.hpp). Task functions check it cooperatively at safe points with
// throw_if_cancelled(); nothing interrupts them otherwise, so a function that
// never checks runs to completion and its result is discarded.
using TaskExecuteFunction = std::function<TaskResult(
    const TaskSpec& spec, const ResolvedInputs& inputs,
    const CancellationToken& cancellation)>;

struct TaskTypeDefinition {
  std::string name;
  std::uint64_t version = 0;
  TaskParameterValidator validate_parameters;
  TaskExecuteFunction execute;
};

// The set of task types compiled into a runtime (plan §4.3: a runtime accepts
// only task types compiled into its registry and validates parameters against
// each type's schema). One version per name: every node in a build runs the
// coordinator's runtime (plan §3.1 rule 2), so two versions of one type never
// meet in a registry.
class TaskTypeRegistry {
 public:
  // Throws ExecError(duplicate_task_type) when the name is already
  // registered, and (invalid_value) for a malformed name, version 0, or a
  // missing function.
  void register_type(TaskTypeDefinition definition);

  // nullptr when the name is unknown or registered with another version.
  [[nodiscard]] const TaskTypeDefinition* find(std::string_view name,
                                               std::uint64_t version) const;

  // Validates the spec (validate_task_spec, including parameters_blake3),
  // resolves its type, and checks its parameters with the type's validator.
  // Throws the validate_task_spec errors, ExecError(unknown_task_type), or
  // (invalid_task_parameters).
  const TaskTypeDefinition& admit(const TaskSpec& spec) const;

  // admit(), then requires `inputs` to hold exactly the spec's inputs with
  // matching refs (ExecError(unresolved_input) otherwise), runs the task with
  // `cancellation`, and validates the result it returns for this task_id
  // (ExecError(invalid_value) when the task_id differs).
  [[nodiscard]] TaskResult execute(const TaskSpec& spec, const ResolvedInputs& inputs,
                                   const CancellationToken& cancellation) const;

 private:
  std::map<std::string, TaskTypeDefinition, std::less<>> types_;
};

}  // namespace svp::exec
