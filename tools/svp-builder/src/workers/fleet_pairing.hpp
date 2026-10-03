#pragma once

// A coordinator pairing every joinable worker of its fleet (fleet_join.hpp)
// that it has not paired yet: no SSH, no password, nobody at the worker.
// `workers fleet pair` runs it and then supplies each new worker as
// `workers pair` does; `build --distributed` runs it before it loads its
// pairings, so a Mac installed with `worker install --join` takes part in the
// next build of every coordinator of the fleet.

#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/pairing_store.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace svp::builder::workers {

struct FleetPairingReport {
  // Pairings recorded by this run.
  std::vector<svp::exec::worker::CoordinatorPairingRecord> paired;
  // Joinable workers this coordinator had already paired.
  std::size_t already_paired = 0;
  // One line per worker that could not be paired.
  std::vector<std::string> problems;
};

// Browses for joinable workers of `membership`'s fleet, pairs each one not
// yet in `store`, and records it there (worker.join_id set, ssh_target
// empty). `runtime_id`/`runtime_kind` are recorded as this coordinator's
// runtime, as `workers pair` does. Progress lines go to `progress` when
// given. Never throws for one worker's failure; browsing failures are
// reported as a problem.
[[nodiscard]] FleetPairingReport pair_joinable_fleet_workers(
    const svp::exec::worker::FleetMembership& membership,
    const svp::exec::worker::PairingDirectory& store, const svp::exec::Blake3Digest& runtime_id,
    svp::exec::worker::RuntimeKind runtime_kind, std::ostream* progress);

}  // namespace svp::builder::workers
