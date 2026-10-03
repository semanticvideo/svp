#include "fleet_pairing.hpp"

#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_id_book.hpp"

#include <set>

namespace svp::builder::workers {

using namespace svp::exec::worker;
namespace remote = svp::exec::remote;

FleetPairingReport pair_joinable_fleet_workers(const FleetMembership& membership,
                                               const PairingDirectory& store,
                                               const svp::exec::Blake3Digest& runtime_id,
                                               RuntimeKind runtime_kind, std::ostream* progress) {
  FleetPairingReport report;
  std::vector<remote::AdvertisedService> services;
  try {
    services = remote::browse_advertised_services(kFleetTxtKey, membership.fleet.fleet_id);
  } catch (const std::exception& error) {
    report.problems.push_back(std::string("browsing for joinable workers failed: ") +
                              error.what());
    return report;
  }
  std::set<std::string> paired_ids;
  for (const CoordinatorPairingRecord& record : load_coordinator_pairings(store)) {
    paired_ids.insert(record.key.pairing_id);
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
    if (paired_ids.contains(pairing_id)) {
      ++report.already_paired;
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
