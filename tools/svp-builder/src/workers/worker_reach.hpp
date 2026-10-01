#pragma once

// Reaching a paired worker for the `workers` commands: discovery by pairing
// id, the HELLO handshake, and model selection from this Mac's cache.

#include "coordinator_context.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/pairing_store.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::workers {

struct WorkerReach {
  bool reachable = false;
  std::optional<svp::exec::worker::WorkerHelloAck> ack;
  std::optional<svp::exec::remote::RouteChoice> route;
  std::string error;
  // False when waiting cannot help: the worker refused the session or does
  // not hold this pairing's secret.
  bool retryable = true;
};

// One connection: HELLO, then SHUTDOWN. Never throws; failures are in
// `error`.
[[nodiscard]] WorkerReach reach_worker(const svp::exec::worker::CoordinatorPairingRecord& record,
                                       const CoordinatorContext& context);

// Retries reach_worker until the worker answers or `timeout` passes;
// a refusal or an authentication failure ends the wait at once.
[[nodiscard]] WorkerReach wait_until_reachable(
    const svp::exec::worker::CoordinatorPairingRecord& record,
    const CoordinatorContext& context, std::chrono::seconds timeout);

// "all", "none", or comma-separated model ids.
[[nodiscard]] std::vector<svp::exec::worker::ModelBundleSource> select_model_bundles(
    const CoordinatorContext& context, const std::string& selection);

// The record whose pairing id or ssh target is `key`. Throws
// WorkerError(configuration) when none or several match.
[[nodiscard]] svp::exec::worker::CoordinatorPairingRecord find_pairing(
    const svp::exec::worker::PairingDirectory& store, const std::string& key);

[[nodiscard]] std::string format_bytes(std::uint64_t bytes);

}  // namespace svp::builder::workers
