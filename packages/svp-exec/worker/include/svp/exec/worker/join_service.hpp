#pragma once

// The worker side of one fleet join connection (fleet_join.hpp), with the
// worker's files: reads the join credential, describes this worker, and
// stores what the join produced (the pairing record 0600 under
// <root>/pairings, and the member key in <root>/join.json) before the
// coordinator is told the join is done.

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <mutex>
#include <string>

namespace svp::exec::worker {

// This Mac as a worker at `layout`: its user, uid, home, root, launchd mode
// (system_daemon when the root is the system root), label, and plist.
[[nodiscard]] WorkerEndpoint describe_this_worker(const WorkerLayout& layout);

struct JoinServiceOutcome {
  std::string pairing_id;
  // The join listener's key changed (the token was replaced by the member
  // key), so it must be restarted with the new one.
  bool listener_key_changed = false;
};

// Serves one join; joins are serialized by `credential_mutex`. Throws
// WorkerError as serve_fleet_join does, or when there is no credential.
JoinServiceOutcome handle_join_connection(FrameReader& input, FrameWriter& output,
                                          const WorkerLayout& layout,
                                          std::mutex& credential_mutex);

}  // namespace svp::exec::worker
