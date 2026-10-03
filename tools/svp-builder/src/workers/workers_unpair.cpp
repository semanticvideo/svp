// `svp-builder workers unpair` (plan §3.3): removes the pairing secret on
// both Macs and, when it was the worker's last pairing, the launchd job, its
// plist, and the worker root (runtimes, models, cache, logs).

#include "calibration_store.hpp"
#include "ssh_session.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_scripts.hpp"
#include "worker_reach.hpp"
#include "workers_cli.hpp"

#include <iostream>

namespace svp::builder::workers {

using namespace svp::exec::worker;

int run_workers_unpair(const WorkersCliOptions& options) {
  const PairingDirectory store(default_coordinator_pairings_dir());
  const CoordinatorPairingRecord record = find_pairing(store, options.worker);
  if (!options.forget && record.worker.ssh_target.empty()) {
    throw WorkerError(WorkerErrorCode::configuration,
                      record.key.pairing_id + " was paired through its fleet join listener, not "
                      "ssh, so this Mac cannot remove it from the worker; `--forget` deletes "
                      "this Mac's pairing record");
  }
  if (!options.forget) {
    const std::string script = render_removal_script(
        WorkerRemoval{.mode = record.worker.service_mode,
                      .root = record.worker.root,
                      .plist = record.worker.plist,
                      .pairing_id = record.key.pairing_id,
                      .label = record.worker.label});
    SshSession ssh(SshTarget{.destination = record.worker.ssh_target,
                             .options = options.ssh_options});
    if (record.worker.service_mode == WorkerServiceMode::user_agent) {
      std::cout << ssh.run_script(script);
    } else {
      std::cout << "removing the LaunchDaemon (sudo asks for the worker's administrator "
                   "password)\n"
                << std::flush;
      ssh.run_interactive("sudo /bin/sh -c " + shell_quote(script));
    }
  }
  store.remove(record.key.pairing_id);
  CalibrationStore().remove(record.key.pairing_id);
  // The last pairing takes the (then empty) store directory with it.
  std::error_code ignored;
  if (std::filesystem::is_empty(store.path(), ignored)) {
    std::filesystem::remove(store.path(), ignored);
    // With no worker left, this Mac's own OCR calibration (kept only for
    // distributed builds) goes too.
    CalibrationStore().remove(std::string(kCoordinatorCalibrationName));
  }
  std::cout << "unpaired " << record.key.pairing_id << " (" << record.worker.ssh_target << ")"
            << (options.forget ? "; the worker was not contacted" : "") << "\n";
  return 0;
}

}  // namespace svp::builder::workers
