// `svp-builder workers fleet init|token|join|pair`: a coordinator's fleet
// secret (fleet_store.hpp), the tokens it issues (fleet_keys.hpp), and
// pairing the fleet's joinable workers without SSH (fleet_pairing.hpp).

#include "coordinator_context.hpp"
#include "fleet_pairing.hpp"
#include "ocr_calibration_runs.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "worker_reach.hpp"
#include "worker_restart.hpp"
#include "workers_cli.hpp"

#include <iostream>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;

// How long a worker a join just paired may take to answer on that pairing:
// it starts the pairing's listener as soon as the join is stored, Bonjour
// registration takes up to kDefaultAdvertiseTimeout (5 s), and each discovery
// attempt waits up to kDefaultDiscoveryTimeout (5 s); 30 s allows a few
// rounds of both on a busy network.
constexpr std::chrono::seconds kJoinedWorkerAnswerTimeout{30};

FleetMembership require_membership() {
  const std::optional<FleetMembership> membership = load_fleet_membership(default_fleet_dir());
  if (!membership) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "this Mac belongs to no fleet; run `svp-builder workers fleet init` (first "
                      "coordinator) or `svp-builder workers fleet join <coordinator token>`");
  }
  return *membership;
}

}  // namespace

int run_workers_fleet_init(const WorkersCliOptions& options) {
  const std::filesystem::path directory = default_fleet_dir();
  if (const std::optional<FleetMembership> existing = load_fleet_membership(directory);
      existing && !options.force) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "this Mac already belongs to fleet " + existing->fleet.fleet_id +
                          "; --force replaces it (workers installed with its tokens can then "
                          "be paired only by coordinators that keep it)");
  }
  const FleetMembership membership{.fleet = generate_fleet_secret(),
                                   .coordinator_id = random_fleet_identifier(kCoordinatorIdPrefix),
                                   .created_at = utc_timestamp_now()};
  save_fleet_membership(directory, membership);
  std::cout << "created fleet " << membership.fleet.fleet_id << " (coordinator "
            << membership.coordinator_id << "); secret stored in "
            << (directory / "fleet.json").string() << "\n"
            << "  new worker Macs: `svp-builder workers fleet token`, then on each Mac "
               "`svp-builder worker install --join <token>`\n"
            << "  other coordinators: `svp-builder workers fleet token --coordinator`, then "
               "`svp-builder workers fleet join -` on each\n";
  return 0;
}

int run_workers_fleet_token(const WorkersCliOptions& options) {
  const FleetMembership membership = require_membership();
  if (options.coordinator_token) {
    std::cerr << "coordinator token for fleet " << membership.fleet.fleet_id
              << ": it is the whole fleet secret and does not expire; give it only to Macs "
                 "that should pair workers\n";
    std::cout << encode_coordinator_token(membership.fleet) << "\n";
    return 0;
  }
  const std::chrono::seconds lifetime =
      options.valid_days == 0
          ? std::chrono::seconds(kDefaultWorkerTokenLifetime)
          : std::chrono::seconds(std::chrono::hours(24) * options.valid_days);
  const WorkerJoinToken token =
      issue_worker_token(membership.fleet, utc_seconds_now(), lifetime);
  std::cerr << "worker token for fleet " << membership.fleet.fleet_id << ", admits new Macs for "
            << lifetime.count() / 3600 / 24 << " day(s); on each new Mac run `svp-builder worker "
            << "install --join -` and paste it\n";
  std::cout << encode_worker_token(token) << "\n";
  return 0;
}

int run_workers_fleet_join(const WorkersCliOptions& options) {
  const FleetSecret fleet = decode_coordinator_token(read_token_argument(options.fleet_token));
  const std::filesystem::path directory = default_fleet_dir();
  const std::optional<FleetMembership> existing = load_fleet_membership(directory);
  if (existing && existing->fleet.fleet_id != fleet.fleet_id && !options.force) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "this Mac already belongs to fleet " + existing->fleet.fleet_id +
                          "; --force leaves it for " + fleet.fleet_id);
  }
  FleetMembership membership;
  membership.fleet = fleet;
  // A coordinator keeps its id when it re-joins its own fleet, so its
  // pairings stay the same.
  membership.coordinator_id = existing && existing->fleet.fleet_id == fleet.fleet_id
                                  ? existing->coordinator_id
                                  : random_fleet_identifier(kCoordinatorIdPrefix);
  membership.created_at = utc_timestamp_now();
  save_fleet_membership(directory, membership);
  std::cout << "joined fleet " << fleet.fleet_id << " as coordinator "
            << membership.coordinator_id << "; `svp-builder workers fleet pair` (or the next "
               "`build --distributed`) pairs its workers\n";
  return 0;
}

int run_workers_fleet_pair(const WorkersCliOptions& options) {
  const FleetMembership membership = require_membership();
  const CoordinatorContext context = load_coordinator_context();
  const std::vector<ModelBundleSource> models = select_model_bundles(context, options.models);
  const PairingDirectory store(default_coordinator_pairings_dir());
  const FleetPairingReport report = pair_joinable_fleet_workers(
      membership, store, context.runtime.runtime_id, context.runtime.kind, &std::cout);
  bool ok = report.problems.empty();
  for (const std::string& problem : report.problems) {
    std::cerr << "svp-builder: " << problem << "\n";
  }
  for (const CoordinatorPairingRecord& record : report.paired) {
    const WorkerReach reach = wait_until_reachable(record, context, kJoinedWorkerAnswerTimeout);
    if (!reach.reachable) {
      std::cerr << "svp-builder: " << record.key.pairing_id
                << " was paired but did not answer: " << reach.error << "\n";
      ok = false;
      continue;
    }
    try {
      const std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
      WorkerSessionClient client(*connection->reader, *connection->writer);
      const WorkerHelloAck ack = client.hello(context.hello());
      TransferStats stats;
      client.ensure_runtime(context.runtime, stats);
      client.ensure_model_bundles(models, stats);
      client.shutdown();
      (void)await_worker_runtime_switch(record.key, context.hello(), context.runtime, ack, nullptr,
                                        [](const std::string& line) { std::cout << line << "\n" << std::flush; });
      std::cout << record.key.pairing_id << ": runtime "
                << (stats.runtime_pushed ? "pushed and verified" : "already present") << ", "
                << stats.model_bundles_pushed.size() << " model bundle(s) pushed ("
                << format_bytes(stats.bytes_sent) << ")\n";
    } catch (const std::exception& error) {
      std::cerr << "svp-builder: supplying " << record.key.pairing_id << " failed: "
                << error.what() << "\n";
      ok = false;
      continue;
    }
    ok = calibrate_for_workers_command(record, context.hello(), context.runtime,
                                       context.model_cache) &&
         ok;
  }
  std::cout << "fleet " << membership.fleet.fleet_id << ": " << report.paired.size()
            << " worker(s) paired now, " << report.already_paired << " already paired\n";
  return ok ? 0 : 1;
}

}  // namespace svp::builder::workers
