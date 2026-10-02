#include "engine/build_audio_dispatch.hpp"

#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/audio/whisper_cpp_backend.hpp"

#include <utility>

namespace svp::builder::engine {

std::unique_ptr<BuildAudioDispatch> make_build_audio_dispatch(BuildAudioDispatchInputs inputs) {
  auto built = std::make_unique<BuildAudioDispatch>();
  StageOutputAccess& outputs = built->outputs;
  // This Mac's tasks: a chunk or window that cannot run here is recorded as
  // not done, so the stage does it itself as a local build does
  // (record_start_failures).
  svp::audio::tasks::register_audio_tasks(
      inputs.registry,
      svp::audio::tasks::AudioTaskEnvironment{
          .model_cache_root = inputs.model_cache_root,
          .model_cache_for = {},
          .scratch_dir = built->scratch.path(),
          .write_output =
              [&outputs](std::span<const std::byte> bytes, std::string media_type,
                         std::string role) {
                return outputs.put(bytes, std::move(media_type), std::move(role));
              },
          .record_start_failures = true});
  built->dispatch = make_audio_work_dispatch(
      std::make_shared<const VisionDispatchSetup>(std::move(inputs.setup)),
      std::move(inputs.extras), inputs.registry, outputs);
  return built;
}

}  // namespace svp::builder::engine
