#pragma once

#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"

#include <filesystem>

namespace svp::builder::workers {

// What the worker side of this runtime provides to task types: everything
// comes from the worker itself, never from the coordinator (plan §4.3: no
// message carries a path).
struct WorkerTaskEnvironment {
  // The session's scratch directory (removed when the session ends).
  std::filesystem::path session_dir;
  // The worker's store of verified model bundles.
  std::filesystem::path model_store;
  // The ffmpeg this runtime decodes with: the runtime bundle's, else
  // $SVP_FFMPEG, else ffmpeg on the job's PATH (runtime_tools.hpp). Tasks
  // still refuse it unless it is the coordinator's build.
  std::filesystem::path ffmpeg_path;
};

// The task types this svp-builder runs for a coordinator (plan §3.1 rule 1:
// each task type is a C++ function in svp-builder, and local and remote
// execution call the same function from the same runtime): ocr.frame_batch
// and the dispatched vision stage work (ocr.crop_batch, embed.text_batch,
// embed.keyframe_batch, depth.frame_batch), track.window, and the audio
// stage work (asr.chunk_batch, diarize.window), with their
// source from the session's content-addressed cache, their models from the
// worker's verified bundles, and their scratch files in the session
// directory. Any other ASSIGN is answered with the
// registry's unknown_task_type failure, never with arbitrary code.
void register_builder_worker_task_types(svp::exec::TaskTypeRegistry& registry,
                                        svp::exec::CasTaskArtifactAccess& artifacts,
                                        const WorkerTaskEnvironment& environment);

}  // namespace svp::builder::workers
