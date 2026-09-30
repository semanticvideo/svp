#include "svp/exec/task_registry.hpp"

#include "record_identifiers.hpp"
#include "svp/exec/exec_error.hpp"

#include <string>
#include <utility>

namespace svp::exec {
namespace {

void require_inputs_resolved(const TaskSpec& spec, const ResolvedInputs& inputs) {
  if (inputs.size() != spec.inputs.size()) {
    throw ExecError(ExecErrorCode::unresolved_input,
                    "task " + spec.task_id + " declares " +
                        std::to_string(spec.inputs.size()) + " inputs but " +
                        std::to_string(inputs.size()) + " were resolved");
  }
  for (const auto& [name, ref] : spec.inputs) {
    const auto resolved = inputs.find(name);
    if (resolved == inputs.end() || resolved->second.ref != ref) {
      throw ExecError(ExecErrorCode::unresolved_input,
                      "task " + spec.task_id + " input `" + name +
                          "` is not resolved to its declared artifact");
    }
  }
}

}  // namespace

void TaskTypeRegistry::register_type(TaskTypeDefinition definition) {
  if (!detail::is_lower_identifier(definition.name) || definition.version == 0 ||
      !definition.validate_parameters || !definition.execute) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task type `" + definition.name +
                        "` needs a lowercase name, a version >= 1, a parameter "
                        "validator, and an execute function");
  }
  if (types_.contains(definition.name)) {
    throw ExecError(ExecErrorCode::duplicate_task_type,
                    "task type `" + definition.name + "` is already registered");
  }
  std::string name = definition.name;
  types_.emplace(std::move(name), std::move(definition));
}

const TaskTypeDefinition* TaskTypeRegistry::find(std::string_view name,
                                                 std::uint64_t version) const {
  const auto iterator = types_.find(name);
  if (iterator == types_.end() || iterator->second.version != version) {
    return nullptr;
  }
  return &iterator->second;
}

const TaskTypeDefinition& TaskTypeRegistry::admit(const TaskSpec& spec) const {
  validate_task_spec(spec);
  const TaskTypeDefinition* definition =
      find(spec.task_type, spec.task_type_version);
  if (definition == nullptr) {
    throw ExecError(ExecErrorCode::unknown_task_type,
                    "task type `" + spec.task_type + "` version " +
                        std::to_string(spec.task_type_version) +
                        " is not registered in this runtime");
  }
  if (const auto reason = definition->validate_parameters(spec.parameters)) {
    throw ExecError(ExecErrorCode::invalid_task_parameters,
                    "task " + spec.task_id + " parameters rejected by `" +
                        spec.task_type + "`: " + *reason);
  }
  return *definition;
}

TaskResult TaskTypeRegistry::execute(const TaskSpec& spec, const ResolvedInputs& inputs,
                                     const CancellationToken& cancellation) const {
  const TaskTypeDefinition& definition = admit(spec);
  require_inputs_resolved(spec, inputs);
  TaskResult result = definition.execute(spec, inputs, cancellation);
  validate_task_result(result);
  if (result.task_id != spec.task_id) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task type `" + spec.task_type + "` returned a result for `" +
                        result.task_id + "` while executing `" + spec.task_id +
                        "`");
  }
  return result;
}

}  // namespace svp::exec
