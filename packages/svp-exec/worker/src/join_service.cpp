#include "svp/exec/worker/join_service.hpp"

#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/worker_error.hpp"

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
  return endpoint;
}

JoinServiceOutcome handle_join_connection(FrameReader& input, FrameWriter& output,
                                          const WorkerLayout& layout,
                                          std::mutex& credential_mutex) {
  const std::lock_guard lock(credential_mutex);
  std::optional<WorkerJoinCredential> credential = load_worker_join_credential(layout);
  if (!credential) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "no join credential at " + layout.join_credential().string());
  }
  const HostFacts host = detect_host_facts();
  JoinServiceOutcome outcome;
  const WorkerJoinResult result = serve_fleet_join(
      input, output, *credential, describe_this_worker(layout), host,
      [&](const WorkerJoinResult& joined) {
        PairingDirectory(layout.pairings())
            .write(joined.pairing.key.pairing_id, encode_worker_pairing(joined.pairing));
        if (credential->member_key != joined.member_key) {
          credential->member_key = joined.member_key;
          save_worker_join_credential(layout, *credential);
          outcome.listener_key_changed = true;
        }
      });
  outcome.pairing_id = result.pairing.key.pairing_id;
  return outcome;
}

}  // namespace svp::exec::worker
