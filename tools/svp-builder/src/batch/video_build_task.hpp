#pragma once

// The worker side of a whole-video job (video.build, M6,
// video_build_parameters.hpp): this Mac's worker service coordinates one
// video of another Mac's batch. The job's session process runs the build
// with the batch Mac's runtime (this process), model bundles (verified in
// this worker's model store), and thread plan, using this Mac's own paired
// workers when the batch is --distributed; the worker service runs as a
// LaunchDaemon, which macOS Local Network privacy lets dial out (plan §3.3).
//
// Everything on disk is this worker's own: the source is given its file
// name under the session's scratch directory, the model cache is a view of
// the named bundles plus the batch Mac's lock, and the package lands in the
// worker's content-addressed cache, pinned until the session ends, for the
// batch to fetch (BLOB_GET).

#include "svp/builder/runtime_tools.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"

#include "../workers/worker_model_view.hpp"

#include <filesystem>
#include <memory>

namespace svp::builder::batch {

struct VideoBuildWorkerEnvironment {
  std::filesystem::path session_dir;
  std::filesystem::path cas_root;
  std::string worker_session_id;
  std::shared_ptr<svp::builder::workers::WorkerModelView> models;
  // This runtime's tools, resolved as a local build of it resolves them.
  RuntimeToolSelection tools;
};

void register_video_build_task(svp::exec::TaskTypeRegistry& registry,
                               svp::exec::CasTaskArtifactAccess& artifacts,
                               VideoBuildWorkerEnvironment environment);

}  // namespace svp::builder::batch
