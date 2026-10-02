#pragma once

// Steps every ONNX-model task type takes before its items run: the model
// cache for the spec, the model loaded from the pool and checked against the
// spec's model ref, and, for types that decode the source, the ffmpeg build
// check. Each step returns the TaskResult to end the attempt with, or
// nothing to go on.

#include "onnx_model_pool.hpp"
#include "task_results.hpp"

#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/tasks/dispatched_task_environment.hpp"
#include "svp/vision/tasks/onnx_model_parameters.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace svp::vision::tasks::detail {

struct OnnxTaskStart {
  // Set when the attempt ends here.
  std::optional<svp::exec::TaskResult> result;
  std::unique_ptr<OnnxModelPool::Lease> model;
};

// `could_not_start(code, reason)` builds the result for a cause another Mac
// may not share (record_start_failures decides what that is).
using CouldNotStart =
    std::function<svp::exec::TaskResult(std::string code, std::string reason)>;

[[nodiscard]] OnnxTaskStart start_onnx_task(const svp::exec::TaskSpec& spec,
                                            std::string_view task_type,
                                            const OnnxModelParameters& model,
                                            bool with_tokenizer,
                                            const DispatchedTaskEnvironment& environment,
                                            OnnxModelPool& models,
                                            const CouldNotStart& could_not_start);

// nullopt when this runtime's ffmpeg is the spec's build; otherwise the
// result to end the attempt with.
[[nodiscard]] std::optional<svp::exec::TaskResult> check_ffmpeg_build(
    const svp::exec::TaskSpec& spec, std::string_view task_type,
    const std::string& ffmpeg_build, const DispatchedTaskEnvironment& environment,
    const CouldNotStart& could_not_start);

}  // namespace svp::vision::tasks::detail
