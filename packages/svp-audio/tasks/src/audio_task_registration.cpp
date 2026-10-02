#include "audio_task_execution.hpp"

#include "svp/audio/tasks/asr_chunk_batch.hpp"
#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/audio/tasks/diarize_window.hpp"

#include <memory>

namespace svp::audio::tasks {

void register_audio_tasks(svp::exec::TaskTypeRegistry& registry,
                          AudioTaskEnvironment environment) {
  auto shared = std::make_shared<const AudioTaskEnvironment>(std::move(environment));
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kAsrChunkBatchTaskType),
      .version = kAsrChunkBatchTaskTypeVersion,
      .validate_parameters = validate_asr_chunk_batch_parameters,
      .execute = [shared](const svp::exec::TaskSpec& spec,
                          const svp::exec::ResolvedInputs& inputs,
                          const svp::exec::CancellationToken& cancellation) {
        return detail::execute_asr_chunk_batch(spec, inputs, cancellation, *shared);
      }});
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kDiarizeWindowTaskType),
      .version = kDiarizeWindowTaskTypeVersion,
      .validate_parameters = validate_diarize_window_parameters,
      .execute = [shared](const svp::exec::TaskSpec& spec,
                          const svp::exec::ResolvedInputs& inputs,
                          const svp::exec::CancellationToken& cancellation) {
        return detail::execute_diarize_window(spec, inputs, cancellation, *shared);
      }});
}

}  // namespace svp::audio::tasks
