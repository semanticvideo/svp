#include "audio_stage.hpp"
#include "engine/stage_tasks.hpp"

namespace svp::builder::engine {
namespace {

constexpr const char* kAudioExtractState = "audio_extract";

}  // namespace

StageStates run_audio_extract_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const AudioExtractStageState extracted = run_audio_extract_stage(task.context());
  StageStates states;
  states[kAudioExtractState] =
      json_state_bytes(audio_extract_stage_state_to_json(extracted));
  return states;
}

StageStates run_audio_transcribe_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const AudioExtractStageState extracted = audio_extract_stage_state_from_json(
      environment.results.json_state(stage_task_id(StageTaskKind::audio_extract),
                                     kAudioExtractState));
  if (const std::optional<int> exit_code =
          run_audio_transcribe_stage(task.context(), extracted, environment.audio_dispatch)) {
    throw StageExitError(*exit_code);
  }
  StageStates states;
  states[state_name::kFoundation] = json_state_bytes(task.output());
  return states;
}

}  // namespace svp::builder::engine
