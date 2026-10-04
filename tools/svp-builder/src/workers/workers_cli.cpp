#include "workers_cli.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <iostream>

namespace svp::builder::workers {

void register_workers_cli(CLI::App& app, WorkersCliOptions& workers, WorkerCliOptions& worker) {
  auto* group = app.add_subcommand("workers", "Pair and manage worker Macs for distributed builds");
  group->require_subcommand(1);

  const auto models_option = [&](CLI::App& command) {
    command.add_option("--models", workers.models,
                       "Model bundles to push from this Mac's model cache: all, none, or a "
                       "comma-separated list of model ids")
        ->capture_default_str();
  };
  const auto ssh_option = [&](CLI::App& command) {
    command.add_option("--ssh-option", workers.ssh_options,
                       "Extra ssh -o option, repeatable (for example IdentityFile=...)");
  };

  workers.pair = group->add_subcommand(
      "pair", "Pair a Mac as a worker: one ssh sign-in installs the runtime and its launchd job");
  workers.pair->add_option("target", workers.pair_target, "Worker as <user>@<host>")->required();
  workers.pair->add_flag("--system-service", workers.system_service,
                         "Install a LaunchDaemon that runs without anyone logged in (needs the "
                         "worker's administrator password for sudo); default: a LaunchAgent");
  models_option(*workers.pair);
  ssh_option(*workers.pair);
  workers.pair->add_flag("--dry-run", workers.dry_run,
                         "Probe the worker and write the plist and scripts locally without "
                         "changing the worker");
  workers.pair->add_option("--dry-run-dir", workers.dry_run_dir,
                           "Where --dry-run writes its files (default: a new temporary directory)");

  workers.list = group->add_subcommand("list", "List paired workers and whether they are reachable");
  workers.list->add_flag("--json", workers.json, "Emit JSON");

  workers.status = group->add_subcommand("status", "Show a paired worker's runtime, OS, and capacity");
  workers.status->add_option("worker", workers.worker, "Pairing id or <user>@<host> (default: all)");
  workers.status->add_flag("--json", workers.json, "Emit JSON");

  workers.sync = group->add_subcommand(
      "sync", "Push this Mac's runtime and model bundles to a paired worker");
  workers.sync->add_option("worker", workers.worker, "Pairing id or <user>@<host>")->required();
  models_option(*workers.sync);

  workers.unpair = group->add_subcommand(
      "unpair", "Remove the worker's launchd job, runtimes, cache, and the pairing secret on both "
                "Macs");
  workers.unpair->add_option("worker", workers.worker, "Pairing id or <user>@<host>")->required();
  ssh_option(*workers.unpair);
  workers.unpair->add_flag("--forget", workers.forget,
                           "Only delete this Mac's pairing record; leave the worker untouched");

  auto* fleet = group->add_subcommand(
      "fleet", "Share a fleet secret so coordinators pair new worker Macs without ssh");
  fleet->require_subcommand(1);
  workers.fleet_init = fleet->add_subcommand(
      "init", "Create this Mac's fleet secret (0600) and become the fleet's first coordinator");
  workers.fleet_init->add_flag("--force", workers.force,
                               "Replace an existing fleet (its worker tokens stop working)");
  workers.fleet_token_command = fleet->add_subcommand(
      "token", "Print a worker token for `svp-builder worker install --join` on a new Mac");
  workers.fleet_token_command->add_flag(
      "--coordinator", workers.coordinator_token,
      "Print a coordinator token for `workers fleet join` instead (the whole fleet secret)");
  workers.fleet_token_command->add_option(
      "--valid-days", workers.valid_days,
      "Days a worker token admits new Macs (default 7, at most 90)");
  workers.fleet_join = fleet->add_subcommand(
      "join", "Join an existing fleet as another coordinator with a coordinator token");
  workers.fleet_join->add_option("token", workers.fleet_token,
                                 "Coordinator token, or - to read it from stdin")
      ->required();
  workers.fleet_join->add_flag("--force", workers.force,
                               "Leave a different fleet this Mac belongs to");
  workers.fleet_pair = fleet->add_subcommand(
      "pair", "Pair every joinable worker of the fleet and push this Mac's runtime and models");
  models_option(*workers.fleet_pair);

  worker.worker = app.add_subcommand("worker", "Worker-side entry points (run by the launchd job)");
  worker.worker->group("");  // Not for interactive use; hidden from --help.
  worker.worker->add_option("--serve-fd", worker.serve_fd,
                            "Serve one coordinator session on this descriptor");
  worker.worker->add_option("--cas-root", worker.cas_root, "Worker content-addressed cache");
  worker.worker->add_option("--session-dir", worker.session_dir, "Session scratch directory");
  worker.worker->add_option("--model-store", worker.model_store,
                            "The worker's verified model bundles");
  worker.worker->add_option("--worker-session-id", worker.worker_session_id, "Session id");
  worker.worker->add_option("--runtime-id", worker.runtime_id, "This runtime's id (b3:<hex>)");
  worker.serve = worker.worker->add_subcommand("serve", "Run the worker agent");
  worker.serve->add_option("--root", worker.root, "Worker root directory")->required();
  worker.serve->add_option("--memory-reserve-floor-mb", worker.memory_reserve_floor_mb,
                           "Override the admission reserve floor (MiB)");
  worker.verify_runtime =
      worker.worker->add_subcommand("verify-runtime", "Verify a runtime directory");
  worker.verify_runtime->add_option("--runtime-dir", worker.runtime_dir, "Runtime directory")
      ->required();
  worker.verify_runtime->add_option("--expect", worker.expect, "Expected runtime id (b3:<hex>)");
  worker.install = worker.worker->add_subcommand(
      "install", "Install this Mac as a fleet worker (LaunchDaemon; asks for the administrator "
                 "password once)");
  worker.install->add_option("--join", worker.join_token,
                             "Worker token from `workers fleet token`, or - to read it from stdin")
      ->required();
  worker.install->add_flag("--dry-run", worker.dry_run,
                           "Stage the runtime, credential, plist, and install script, and print "
                           "the sudo command instead of running it");
  worker.install->add_option("--dry-run-dir", worker.dry_run_dir,
                             "Where --dry-run stages (default: a new temporary directory)");
}

std::string read_token_argument(const std::string& argument) {
  if (argument != "-") {
    return argument;
  }
  std::string line;
  std::getline(std::cin, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  return line;
}

std::optional<int> run_workers_cli(const WorkersCliOptions& workers,
                                   const WorkerCliOptions& worker) {
  try {
    if (workers.pair && *workers.pair) return run_workers_pair(workers);
    if (workers.list && *workers.list) return run_workers_list(workers);
    if (workers.status && *workers.status) return run_workers_status(workers);
    if (workers.sync && *workers.sync) return run_workers_sync(workers);
    if (workers.unpair && *workers.unpair) return run_workers_unpair(workers);
    if (workers.fleet_init && *workers.fleet_init) return run_workers_fleet_init(workers);
    if (workers.fleet_token_command && *workers.fleet_token_command) {
      return run_workers_fleet_token(workers);
    }
    if (workers.fleet_join && *workers.fleet_join) return run_workers_fleet_join(workers);
    if (workers.fleet_pair && *workers.fleet_pair) return run_workers_fleet_pair(workers);
    if (worker.serve && *worker.serve) return run_worker_serve(worker);
    if (worker.verify_runtime && *worker.verify_runtime) return run_worker_verify_runtime(worker);
    if (worker.install && *worker.install) return run_worker_install(worker);
    if (worker.worker && *worker.worker) {
      if (worker.serve_fd < 0) {
        std::cerr << "svp-builder worker: use `worker serve`, `worker verify-runtime`, or "
                     "--serve-fd\n";
        return 2;
      }
      return run_worker_session(worker);
    }
  } catch (const svp::exec::worker::WorkerError& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  } catch (const svp::exec::remote::RemoteTransportError& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  }
  return std::nullopt;
}

}  // namespace svp::builder::workers
