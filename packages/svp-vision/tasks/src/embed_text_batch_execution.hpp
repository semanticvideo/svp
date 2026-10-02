#pragma once

#include "onnx_model_pool.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/resolved_inputs.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/tasks/dispatched_task_environment.hpp"

namespace svp::vision::tasks::detail {

// One embed.text_batch attempt (onnx_task_registration.hpp).
[[nodiscard]] svp::exec::TaskResult execute_embed_text_batch(
    const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
    const svp::exec::CancellationToken& cancellation,
    const DispatchedTaskEnvironment& environment, OnnxModelPool& models);

}  // namespace svp::vision::tasks::detail
