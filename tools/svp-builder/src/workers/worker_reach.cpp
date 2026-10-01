#include "worker_reach.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <cstdio>
#include <sstream>
#include <thread>

namespace svp::builder::workers {

using namespace svp::exec::worker;
namespace remote = svp::exec::remote;

namespace {

// Pause between attempts while a freshly started job comes up; one
// discovery settle period (route_policy.hpp), so mDNS sees the new record.
constexpr std::chrono::milliseconds kReachRetryPause{500};

}  // namespace

WorkerReach reach_worker(const CoordinatorPairingRecord& record, const CoordinatorContext& context) {
  WorkerReach reach;
  try {
    const std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
    reach.route = connection->connection.route;
    WorkerSessionClient client(*connection->reader, *connection->writer);
    reach.ack = client.hello(context.hello());
    client.shutdown();
    reach.reachable = true;
  } catch (const WorkerError& error) {
    reach.error = error.what();
    reach.retryable = error.code() != WorkerErrorCode::refused;
  } catch (const remote::RemoteTransportError& error) {
    reach.error = std::string(remote::remote_error_code_name(error.code())) + ": " + error.what();
    reach.retryable = error.code() != remote::RemoteErrorCode::authentication_failed;
  } catch (const std::exception& error) {
    reach.error = error.what();
  }
  return reach;
}

WorkerReach wait_until_reachable(const CoordinatorPairingRecord& record,
                                 const CoordinatorContext& context, std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    WorkerReach reach = reach_worker(record, context);
    if (reach.reachable || !reach.retryable || std::chrono::steady_clock::now() + kReachRetryPause >= deadline) {
      return reach;
    }
    std::this_thread::sleep_for(kReachRetryPause);
  }
}

std::vector<ModelBundleSource> select_model_bundles(const CoordinatorContext& context,
                                                    const std::string& selection) {
  if (selection == "none") {
    return {};
  }
  std::vector<std::string> ids;
  if (selection != "all") {
    std::stringstream list(selection);
    std::string id;
    while (std::getline(list, id, ',')) {
      if (!id.empty()) {
        ids.push_back(id);
      }
    }
    if (ids.empty()) {
      throw WorkerError(WorkerErrorCode::configuration,
                        "--models wants all, none, or a comma-separated list of model ids");
    }
  }
  return prepare_model_bundles(context.model_cache, ids);
}

CoordinatorPairingRecord find_pairing(const PairingDirectory& store, const std::string& key) {
  std::vector<CoordinatorPairingRecord> matches;
  for (CoordinatorPairingRecord& record : load_coordinator_pairings(store)) {
    if (record.key.pairing_id == key || record.worker.ssh_target == key) {
      matches.push_back(std::move(record));
    }
  }
  if (matches.empty()) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "no paired worker matches `" + key + "` in " + store.path().string() +
                          " (see `svp-builder workers list`)");
  }
  if (matches.size() > 1) {
    std::string ids;
    for (const CoordinatorPairingRecord& record : matches) {
      ids += " " + record.key.pairing_id;
    }
    throw WorkerError(WorkerErrorCode::configuration,
                      "`" + key + "` matches several pairings; name one by id:" + ids);
  }
  return std::move(matches.front());
}

std::string format_bytes(std::uint64_t bytes) {
  char text[32];
  if (bytes >= (1ULL << 30)) {
    std::snprintf(text, sizeof(text), "%.1f GiB", static_cast<double>(bytes) / (1ULL << 30));
  } else {
    std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1ULL << 20));
  }
  return text;
}

}  // namespace svp::builder::workers
