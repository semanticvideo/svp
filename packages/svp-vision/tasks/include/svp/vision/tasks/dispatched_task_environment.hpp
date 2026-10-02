#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_spec.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace svp::vision::tasks {

// What a runtime provides to the dispatched vision task types
// (ocr.crop_batch, embed.text_batch, embed.keyframe_batch,
// depth.frame_batch) beyond the TaskSpec: its own model cache, ffmpeg, and
// scratch space (never sent by the coordinator, plan §4.3: no message carries
// a path), and the store its outputs go to.
struct DispatchedTaskEnvironment {
  // The model cache models load from, unless model_cache_for is set.
  std::filesystem::path model_cache_root;
  // Optional: the model cache for one spec (a worker's view over exactly the
  // bundles the spec's model_refs name). Throwing means this runtime cannot
  // provide those bundles now.
  std::function<std::filesystem::path(const svp::exec::TaskSpec& spec)> model_cache_for;
  // The ffmpeg this runtime decodes and encodes with. Its
  // ffmpeg_build_identity() must equal the spec's decode.ffmpeg_build.
  std::filesystem::path ffmpeg_path;
  // Where tasks write temporary files (crop images before they are read
  // back). Must exist.
  std::filesystem::path scratch_dir;
  // Stores output bytes in the runtime's TaskArtifactAccess store and returns
  // their ref.
  std::function<svp::exec::ArtifactRef(std::span<const std::byte> bytes,
                                       std::string media_type, std::string role)>
      write_output;
  // True only on the coordinator: a task that cannot start here (its model
  // does not load, ffmpeg cannot run) succeeds with every item failed, so the
  // stage does that work itself exactly as a build without tasks does
  // (svp/vision/dispatched_work.hpp). False (workers): a retryable failure,
  // so the task runs on another Mac and output never depends on which Mac
  // took it.
  bool record_start_failures = false;
};

}  // namespace svp::vision::tasks
