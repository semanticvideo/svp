#pragma once

// A worker's id: what its own Bonjour instance advertises (TXT
// kWorkerTxtKey, transport_policy.hpp) and HELLO_ACK reports, so
// coordinators find the worker by it, whatever pairings it serves.
// "svpn-" + 24 random hex digits, created once and kept in
// <root>/worker-id.json (0600):
//   {"schema":"svp.worker.id/1","worker_id":"svpn-<24 hex>"}

#include "svp/exec/worker/worker_layout.hpp"

#include <string>
#include <string_view>

namespace svp::exec::worker {

inline constexpr std::string_view kWorkerIdPrefix = "svpn-";
inline constexpr std::string_view kWorkerIdSchema = "svp.worker.id/1";

// The worker id stored under `layout`, created when there is none. Throws
// WorkerError(io, configuration).
[[nodiscard]] std::string load_or_create_worker_id(const WorkerLayout& layout);

}  // namespace svp::exec::worker
