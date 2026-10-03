#pragma once

// The worker side of one fleet join connection (fleet_join.hpp), with the
// worker's files: reads the join credential, describes this worker, and
// stores what the join produced (the pairing record 0600 under
// <root>/pairings, and the member key in <root>/join.json) before the
// coordinator is told the join is done.

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <mutex>
#include <optional>
#include <vector>
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

// The join credential under `layout`, migrated (and saved) when it predates
// worker keys (fleet_store.hpp migrate_join_credential); nullopt without one.
// Reads and writes under `credential_mutex`.
[[nodiscard]] std::optional<WorkerJoinCredential> load_current_join_credential(
    const WorkerLayout& layout, std::mutex& credential_mutex);

// The worker's fleet state for HELLO_ACK (hello_messages.hpp `fleet`);
// nullopt without a join credential.
[[nodiscard]] std::optional<FleetJoinState> current_fleet_state(const WorkerLayout& layout,
                                                                std::mutex& credential_mutex);

// Stores the member key a proven coordinator issued over a pairing session
// (fleet_member_messages.hpp) when `join_id` is this worker's join id.
// Returns "" when stored (sets `changed` when it differs from the key held,
// so the join listener must restart with it), else why not.
[[nodiscard]] std::string store_issued_member_key(const WorkerLayout& layout,
                                                  std::mutex& credential_mutex,
                                                  const std::string& join_id,
                                                  const std::vector<std::byte>& member_key,
                                                  bool& changed);

// Serves one join. `credential_mutex` guards the credential file only while
// it is read and while the result is stored, never across the exchange, so a
// stalled peer blocks nothing else (the caller bounds the exchange with
// kJoinExchangeTimeout). Throws WorkerError as serve_fleet_join does, or when
// there is no credential.
JoinServiceOutcome handle_join_connection(FrameReader& input, FrameWriter& output,
                                          const WorkerLayout& layout,
                                          std::mutex& credential_mutex);

}  // namespace svp::exec::worker
