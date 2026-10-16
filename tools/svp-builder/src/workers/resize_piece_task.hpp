#pragma once

// The worker side of resize.piece (resize/piece_task.hpp): encodes one piece
// of another Mac's split resize with piece_encode_command, after checking
// that this Mac's ffmpeg is the coordinator's build, and keeps the encoded
// piece in the worker's content-addressed cache, pinned until the session
// ends, for the coordinator to fetch.

#include "cas_output_pins.hpp"

#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"

#include <filesystem>
#include <string>

namespace svp::builder::workers {

struct ResizePieceWorkerEnvironment {
  // The session's scratch directory (removed when the session ends).
  std::filesystem::path session_dir;
  std::filesystem::path cas_root;
  std::string worker_session_id;
  // The ffmpeg this runtime encodes with.
  std::filesystem::path ffmpeg_path;
};

void register_resize_piece_task(svp::exec::TaskTypeRegistry& registry,
                                svp::exec::CasTaskArtifactAccess& artifacts,
                                ResizePieceWorkerEnvironment environment);

}  // namespace svp::builder::workers
