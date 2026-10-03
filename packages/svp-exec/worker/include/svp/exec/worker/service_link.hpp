#pragma once

// The worker service's stable entry point (worker_layout.hpp):
//
//   <root>/current -> runtimes/<runtime hex>
//
// a symlink with a RELATIVE target, owned by the worker's user. The launchd
// job runs <root>/current/bin/svp-builder, so moving the service to another
// runtime needs no change to its root-owned plist: the service repoints the
// link itself (service_updater.hpp) and exits for launchd to restart it.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace svp::exec::worker {

// "runtimes/<hex>": the link target for `runtime_id`.
[[nodiscard]] std::string current_link_target(const Blake3Digest& runtime_id);

// The runtime <root>/current names; nullopt when the link is missing or is
// not a symlink to runtimes/<64 hex>.
[[nodiscard]] std::optional<Blake3Digest> read_current_runtime(const WorkerLayout& layout);

// Points <root>/current at runtimes/<hex> atomically: a new link is made
// under a temporary name in the root and renamed over `current`, so a
// launchd start at any moment finds either the old target or the new one.
// Throws WorkerError(io).
void point_current_at(const WorkerLayout& layout, const Blake3Digest& runtime_id);

// Whether `launched_program` (the program path as the process was started,
// symlinks not resolved) is <root>/current/bin/svp-builder. Only a service
// started through the link can move itself to another runtime: one whose
// plist names a fixed runtime would restart on that same runtime forever.
[[nodiscard]] bool launched_through_current(const WorkerLayout& layout,
                                            const std::filesystem::path& launched_program);

}  // namespace svp::exec::worker
