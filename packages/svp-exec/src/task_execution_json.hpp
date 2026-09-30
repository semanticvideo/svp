#pragma once

#include "svp/exec/task_execution.hpp"

#include <nlohmann/json.hpp>
#include <string_view>

namespace svp::exec::detail {

void validate_task_execution(const TaskExecution& execution);

[[nodiscard]] nlohmann::json task_execution_to_json(
    const TaskExecution& execution);

[[nodiscard]] TaskExecution task_execution_from_json_at(
    const nlohmann::json& value, std::string_view path);

}  // namespace svp::exec::detail
