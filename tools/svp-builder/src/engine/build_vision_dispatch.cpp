#include "engine/build_vision_dispatch.hpp"

#include "engine/vision_work_dispatch.hpp"

#include "svp/vision/tasks/dispatched_vision_tasks.hpp"

#include <utility>

namespace svp::builder::engine {

std::unique_ptr<BuildVisionDispatch> make_build_vision_dispatch(
    BuildVisionDispatchInputs inputs) {
  auto built = std::make_unique<BuildVisionDispatch>();
  StageOutputAccess& outputs = built->outputs;
  outputs.register_input(inputs.setup.source, inputs.source_path);
  // This Mac's tasks: a start failure here is every item failed, so the
  // stage does that work itself as a local build does
  // (record_start_failures).
  inputs.setup.release_idle_models = svp::vision::tasks::register_dispatched_vision_tasks(
      inputs.registry,
      svp::vision::tasks::DispatchedTaskEnvironment{
          .model_cache_root = inputs.model_cache_root,
          .model_cache_for = {},
          .ffmpeg_path = inputs.ffmpeg_path,
          .scratch_dir = built->scratch.path(),
          .write_output =
              [&outputs](std::span<const std::byte> bytes, std::string media_type,
                         std::string role) {
                return outputs.put(bytes, std::move(media_type), std::move(role));
              },
          .record_start_failures = true},
      std::move(inputs.pp_ocr_sessions));
  built->dispatch = make_vision_work_dispatch(
      std::make_shared<const VisionDispatchSetup>(std::move(inputs.setup)), inputs.registry,
      outputs);
  return built;
}

}  // namespace svp::builder::engine
