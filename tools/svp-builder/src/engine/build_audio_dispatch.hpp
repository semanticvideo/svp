#pragma once

// Sets up a --distributed build's audio dispatch (audio_work_dispatch.hpp,
// M5): registers the audio task types for this Mac's own slots, gives them a
// scratch directory and an output store, and makes the transcription
// stage's dispatchers. Builds that dispatch no audio work never call this.

#include "engine/audio_work_dispatch.hpp"
#include "engine/dispatch_scratch.hpp"
#include "engine/stage_output_access.hpp"
#include "engine/vision_dispatch_setup.hpp"

#include "svp/exec/task_registry.hpp"

#include <filesystem>
#include <memory>

namespace svp::builder::engine {

struct BuildAudioDispatch {
  // Where this Mac's audio tasks write chunk slices; removed with this
  // object, after the build.
  DispatchScratch scratch;
  // This Mac's audio tasks' WAVs and outputs. Outputs are released once
  // read back: the stage takes them straight from the scheduler.
  StageOutputAccess outputs{StageOutputRetention::release_after_read};
  AudioWorkDispatch dispatch;
};

struct BuildAudioDispatchInputs {
  svp::exec::TaskTypeRegistry& registry;
  // This Mac's model cache.
  std::filesystem::path model_cache_root;
  // The dispatch setup (its source and decoder are not used by audio).
  VisionDispatchSetup setup;
  AudioDispatchExtras extras;
};

[[nodiscard]] std::unique_ptr<BuildAudioDispatch> make_build_audio_dispatch(
    BuildAudioDispatchInputs inputs);

}  // namespace svp::builder::engine
