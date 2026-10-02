#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/tasks/track_window_runtime_pool.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace svp::vision::tasks {

// What a runtime provides to track.window beyond the TaskSpec: its own model
// cache and ffmpeg (never sent by the coordinator, plan §4.3: no message
// carries a path), and the store its outputs go to.
struct TrackWindowWorkerEnvironment {
  // The model cache the window runtimes load from, unless model_cache_for is
  // set.
  std::filesystem::path model_cache_root;
  // The ffmpeg this runtime decodes with. Its ffmpeg_build_identity() must
  // equal the spec's decode.ffmpeg_build.
  std::filesystem::path ffmpeg_path;
  // Stores output bytes in the runtime's TaskArtifactAccess store and returns
  // their ref.
  std::function<svp::exec::ArtifactRef(std::span<const std::byte> bytes,
                                       std::string media_type, std::string role)>
      write_output;
  // Optional: the model cache for one spec (a worker's view over exactly the
  // bundles the spec's model_refs name). Throwing means this runtime cannot
  // provide those bundles now.
  std::function<std::filesystem::path(const svp::exec::TaskSpec& spec)> model_cache_for;
  // True only on the coordinator: a window that cannot start here (ffmpeg
  // cannot run, a runtime does not load) succeeds with a not_started outcome,
  // so the tracking stage falls back to running exactly as a build without
  // window tasks does. False (workers): a retryable failure, so the window
  // runs on another Mac and output never depends on which Mac took it.
  bool record_start_failures = false;
};

// Registers track.window version 1 with its strict parameter validator.
//
// The task decodes the spec's window from its `source` input and runs the
// window (svp::vision::run_visual_entity_window) with the parameters' frame
// IDs, returning one encoded VisualEntityWindowOutcome
// (kTrackWindowOutcomeRole). Before running it requires the spec's
// model_refs to name exactly the detector, depth, and embedding models of the
// parameters, and the loaded detector and depth bundles to match those refs.
// Runtimes are pooled for the registry's lifetime (TrackWindowRuntimePool).
// Cancellation is checked between frames.
//
// Failures another worker may not share are retryable failed results:
// tracking_unavailable (a runtime did not load here), model_unavailable (this
// runtime cannot provide the named bundles), model_mismatch (this worker's
// bundles differ from the refs), decode_unavailable (no usable ffmpeg here),
// decoder_mismatch (this runtime's ffmpeg is another build than the spec's).
// A spec whose model_refs disagree with its parameters is invalid_model_refs,
// permanent. On the coordinator, decode misses and detector, depth, and
// tracker errors are data in the outcome, as they are for a local build; on a
// worker a window with any of them is window_failed_here, retryable, so it
// runs elsewhere and in the end on the coordinator.
//
// `runtimes` is the pool tasks take runtimes from; null gives the
// registration a pool of its own.
void register_track_window_task(svp::exec::TaskTypeRegistry& registry,
                                TrackWindowWorkerEnvironment environment,
                                std::shared_ptr<TrackWindowRuntimePool> runtimes = nullptr);

}  // namespace svp::vision::tasks
