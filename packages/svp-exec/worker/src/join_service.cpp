#include "svp/exec/worker/join_service.hpp"

#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_identity.hpp"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace svp::exec::worker {

WorkerEndpoint describe_this_worker(const WorkerLayout& layout) {
  WorkerEndpoint endpoint;
  endpoint.uid = static_cast<std::uint32_t>(::getuid());
  if (const passwd* entry = ::getpwuid(::getuid()); entry != nullptr) {
    endpoint.user = entry->pw_name;
    endpoint.home = entry->pw_dir;
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    endpoint.home = home;
  }
  endpoint.root = layout.root.string();
  endpoint.service_mode =
      layout.root == default_worker_root(WorkerServiceMode::system_daemon, endpoint.home)
          ? WorkerServiceMode::system_daemon
          : WorkerServiceMode::user_agent;
  endpoint.label = std::string(kWorkerJobLabel);
  endpoint.plist = launchd_plist_path(endpoint.service_mode, endpoint.home).string();
  endpoint.worker_id = load_or_create_worker_id(layout);
  return endpoint;
}

std::optional<WorkerJoinCredential> load_current_join_credential(const WorkerLayout& layout,
                                                                std::mutex& credential_mutex) {
  const std::lock_guard lock(credential_mutex);
  std::optional<WorkerJoinCredential> credential = load_worker_join_credential(layout);
  if (credential && migrate_join_credential(*credential)) {
    save_worker_join_credential(layout, *credential);
  }
  return credential;
}

std::optional<FleetJoinState> current_fleet_state(const WorkerLayout& layout,
                                                  std::mutex& credential_mutex) {
  try {
    const std::lock_guard lock(credential_mutex);
    const std::optional<WorkerJoinCredential> credential = load_worker_join_credential(layout);
    if (!credential) {
      return std::nullopt;
    }
    return FleetJoinState{.fleet_id = credential->token.fleet_id,
                          .join_id = credential->worker_join_id,
                          .member = credential->member_key.has_value()};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string store_issued_member_key(const WorkerLayout& layout, std::mutex& credential_mutex,
                                    const std::string& join_id,
                                    const std::vector<std::byte>& member_key, bool& changed) {
  changed = false;
  try {
    const std::lock_guard lock(credential_mutex);
    std::optional<WorkerJoinCredential> credential = load_worker_join_credential(layout);
    if (!credential) {
      return "this worker has no fleet join credential";
    }
    if (credential->worker_join_id != join_id) {
      return "this worker's join id is " + credential->worker_join_id + ", not " + join_id;
    }
    if (credential->member_key != member_key) {
      credential->member_key = member_key;
      save_worker_join_credential(layout, *credential);
      changed = true;
    }
    return {};
  } catch (const std::exception& error) {
    return std::string("cannot store the member key: ") + error.what();
  }
}

JoinServiceOutcome handle_join_connection(FrameReader& input, FrameWriter& output,
                                          const WorkerLayout& layout,
                                          std::mutex& credential_mutex) {
  // The credential is read under the lock and the exchange runs without it,
  // so a peer that stalls never blocks other joins or the join listener's
  // restart; only storing the result takes the lock again.
  const std::optional<WorkerJoinCredential> credential =
      load_current_join_credential(layout, credential_mutex);
  if (!credential) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "no join credential at " + layout.join_credential().string());
  }
  const HostFacts host = detect_host_facts();
  JoinServiceOutcome outcome;
  const WorkerJoinResult result = serve_fleet_join(
      input, output, *credential, describe_this_worker(layout), host,
      [&](const WorkerJoinResult& joined) {
        const std::lock_guard lock(credential_mutex);
        PairingDirectory(layout.pairings())
            .write(joined.pairing.key.pairing_id, encode_worker_pairing(joined.pairing));
        std::optional<WorkerJoinCredential> current = load_worker_join_credential(layout);
        if (current && current->worker_join_id == credential->worker_join_id &&
            current->member_key != joined.member_key) {
          current->member_key = joined.member_key;
          save_worker_join_credential(layout, *current);
          outcome.listener_key_changed = true;
        }
      });
  outcome.pairing_id = result.pairing.key.pairing_id;
  return outcome;
}

}  // namespace svp::exec::worker
