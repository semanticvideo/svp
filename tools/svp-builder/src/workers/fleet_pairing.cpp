#include "fleet_pairing.hpp"

#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/stream_deadline.hpp"
#include "svp/exec/remote/transport_policy.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/fleet_member_messages.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_id_book.hpp"

#include <map>
#include <set>

namespace svp::builder::workers {

using namespace svp::exec::worker;
namespace remote = svp::exec::remote;

namespace {

// Over `record`'s proven pairing session: issues the worker its member key
// when its HELLO_ACK shows it lacks one. True when issued.
bool issue_member_key(const CoordinatorPairingRecord& record, const FleetMembership& membership,
                      const CoordinatorHello& hello) {
  const std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
  const remote::StreamDeadline deadline(*connection->connection.stream, kJoinExchangeTimeout);
  WorkerSessionClient client(*connection->reader, *connection->writer);
  const WorkerHelloAck ack = client.hello(hello);
  bool issued = false;
  if (worker_needs_member_key(ack, membership.fleet.fleet_id)) {
    // The join id comes from the worker over this authenticated session,
    // never from an advertisement.
    client.issue_member_key(ack.fleet->join_id, member_key(membership.fleet, ack.fleet->join_id));
    issued = true;
  }
  client.shutdown();
  return issued;
}

}  // namespace

FleetPairingReport pair_joinable_fleet_workers(const FleetMembership& membership,
                                               const PairingDirectory& store,
                                               const svp::exec::Blake3Digest& runtime_id,
                                               RuntimeKind runtime_kind,
                                               const CoordinatorHello& hello,
                                               std::ostream* progress) {
  FleetPairingReport report;
  std::vector<remote::AdvertisedService> services;
  try {
    services = remote::browse_advertised_services(kFleetTxtKey, membership.fleet.fleet_id);
  } catch (const std::exception& error) {
    report.problems.push_back(std::string("browsing for joinable workers failed: ") +
                              error.what());
    return report;
  }
  std::map<std::string, CoordinatorPairingRecord> paired_ids;
  std::map<std::string, CoordinatorPairingRecord> paired_workers;
  for (const CoordinatorPairingRecord& record : load_coordinator_pairings(store)) {
    paired_ids.emplace(record.key.pairing_id, record);
    if (!record.worker.worker_id.empty()) {
      paired_workers.emplace(record.worker.worker_id, record);
    }
  }
  std::set<std::string> seen;
  const std::uint64_t now = utc_seconds_now();
  for (const remote::AdvertisedService& service : services) {
    const auto join_id = service.txt.find(std::string(remote::kPairingTxtKey));
    const auto join_txt = service.txt.find(std::string(kJoinTxtKey));
    if (join_id == service.txt.end() || join_txt == service.txt.end() ||
        !seen.insert(join_id->second).second) {
      continue;
    }
    const std::string pairing_id = fleet_pairing_id(membership.coordinator_id, join_id->second);
    // A worker this coordinator already pairs, under an earlier join id (a
    // credential migrated to a key-bound id, fleet_store.hpp): one pairing
    // per worker, so a build never counts a worker twice.
    const auto worker = service.txt.find(std::string(remote::kWorkerTxtKey));
    const CoordinatorPairingRecord* known = nullptr;
    if (const auto found = paired_ids.find(pairing_id); found != paired_ids.end()) {
      known = &found->second;
    } else if (worker != service.txt.end()) {
      if (const auto found_worker = paired_workers.find(worker->second);
          found_worker != paired_workers.end()) {
        known = &found_worker->second;
      }
    }
    if (known != nullptr) {
      ++report.already_paired;
      // Still advertising its token: it holds no member key (a credential
      // migrated to a key-bound join id drops the old one). Issue it over
      // the pairing this coordinator already holds, so it needs neither the
      // join listener nor a valid token (fleet_member_messages.hpp).
      if (join_txt->second != kJoinTxtMember) {
        try {
          if (issue_member_key(*known, membership, hello)) {
            ++report.member_keys_issued;
            if (progress != nullptr) {
              *progress << "issued the fleet member key to " << known->key.pairing_id << "\n";
            }
          }
        } catch (const std::exception& error) {
          report.problems.push_back(known->key.pairing_id +
                                    ": cannot issue its member key: " + error.what());
        }
      }
      continue;
    }
    const std::string name = "`" + service.service_name + "` (" + join_id->second + ")";
    const JoinListenerKey key =
        coordinator_join_key(membership.fleet, join_id->second, join_txt->second, now);
    if (!key.key) {
      report.problems.push_back(name + ": " + key.refusal);
      continue;
    }
    try {
      const std::unique_ptr<WorkerConnection> connection = connect_to_worker(*key.key);
      // A worker that stalls mid-join is cut off instead of holding the
      // command (fleet_join.hpp kJoinExchangeTimeout).
      const remote::StreamDeadline deadline(*connection->connection.stream, kJoinExchangeTimeout);
      const CoordinatorJoinResult joined = join_fleet_worker(*connection->reader,
                                                             *connection->writer, membership,
                                                             join_id->second);
      CoordinatorPairingRecord record;
      record.key = joined.key;
      record.created_at = utc_timestamp_now();
      record.runtime_id = runtime_id;
      record.runtime_kind = runtime_kind;
      record.worker = joined.worker;
      store.write(record.key.pairing_id, encode_coordinator_pairing(record));
      default_worker_id_book().learn(record.key.pairing_id, record.worker.worker_id);
      if (progress != nullptr) {
        *progress << "paired " << name << " as " << record.key.pairing_id << " ("
                  << record.worker.user << ", macOS " << record.worker.os.product_version
                  << ") through fleet " << membership.fleet.fleet_id << "\n";
      }
      report.paired.push_back(std::move(record));
    } catch (const remote::RemoteTransportError& error) {
      report.problems.push_back(name + ": " +
                                std::string(remote::remote_error_code_name(error.code())) + ": " +
                                error.what());
    } catch (const std::exception& error) {
      report.problems.push_back(name + ": " + error.what());
    }
  }
  return report;
}

}  // namespace svp::builder::workers
