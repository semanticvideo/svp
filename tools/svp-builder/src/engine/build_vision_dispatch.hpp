#pragma once

// Sets up a --distributed build's vision dispatch (vision_dispatch_setup.hpp,
// M4): registers the dispatched task types for this Mac's own slots, gives
// them a scratch directory and the build's output store, and makes the
// stages' dispatchers. Builds that dispatch nothing never call this.

#include "engine/dispatch_scratch.hpp"
#include "engine/stage_output_access.hpp"
#include "engine/vision_dispatch_setup.hpp"

#include "svp/exec/task_registry.hpp"
#include "svp/package/vision_lane_stages.hpp"
#include "svp/vision/tasks/pp_ocr_session_pool.hpp"

#include <filesystem>
#include <memory>

namespace svp::builder::engine {

struct BuildVisionDispatch {
  // Where this Mac's dispatched tasks write temporary files; removed with
  // this object, after the build.
  DispatchScratch scratch;
  svp::package::VisionWorkDispatch dispatch;
};

struct BuildVisionDispatchInputs {
  // Holds the build's source (registered by the caller), and receives task
  // outputs.
  StageOutputAccess& outputs;
  svp::exec::TaskTypeRegistry& registry;
  // This Mac's model cache and ffmpeg.
  std::filesystem::path model_cache_root;
  std::filesystem::path ffmpeg_path;
  // The PP-OCR pool the OCR stage uses; crop tasks share it.
  std::shared_ptr<svp::vision::tasks::PpOcrSessionPool> pp_ocr_sessions;
  // Everything else the dispatchers need; release_idle_models is filled in.
  VisionDispatchSetup setup;
};

[[nodiscard]] std::unique_ptr<BuildVisionDispatch> make_build_vision_dispatch(
    BuildVisionDispatchInputs inputs);

}  // namespace svp::builder::engine
