#pragma once

#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/resolved_inputs.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

namespace svp::audio::tasks::detail {

[[nodiscard]] svp::exec::TaskResult execute_asr_chunk_batch(
    const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
    const svp::exec::CancellationToken& cancellation, const AudioTaskEnvironment& environment);

[[nodiscard]] svp::exec::TaskResult execute_diarize_window(
    const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
    const svp::exec::CancellationToken& cancellation, const AudioTaskEnvironment& environment);

}  // namespace svp::audio::tasks::detail
