// `svp-builder workers pair <user>@<host>` (plan §3.3).
//
//   1. Probe the worker over the system ssh: arm64, the coordinator's macOS
//      product version, free disk, and no SVP job of the other launchd mode.
//   2. Stage this Mac's runtime locally in the worker layout and stream it
//      with tar; the worker's copy of svp-builder verifies every file's
//      BLAKE3 against the manifest before the runtime is moved into place.
//   3. Write a fresh 256-bit pairing secret on the worker (0600, over ssh's
//      stdin, never on a command line).
//   4. Point <root>/current at the runtime (unless it already names one,
//      service_link.hpp) and install the launchd job, which runs
//      <root>/current/bin/svp-builder: a LaunchAgent in the user's domain, or
//      with --system-service a LaunchDaemon with UserName set, installed by a
//      staged script run under sudo in a terminal session. From then on the
//      service moves itself to newer runtimes (service_updater.hpp).
//   5. Record the pairing on this Mac (0600), wait until the worker answers
//      HELLO by pairing id, then push the selected model bundles.

#include "coordinator_context.hpp"
#include "ssh_session.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/launchd_job.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_scripts.hpp"
#include "ocr_calibration_runs.hpp"
#include "worker_reach.hpp"
#include "workers_cli.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;

// Free disk a worker needs beyond the runtime and model bundles: room for a
// first build's blobs to land (plan §3.2 measured 565 MB of source and 20 MB
// of analysis audio for the reference fixture) without filling the volume.
constexpr std::uint64_t kPairingDiskHeadroomBytes = 2ULL * 1024 * 1024 * 1024;

// How long a freshly loaded job may take to answer HELLO: launchd starts it
// at bootstrap, the agent registers its Bonjour service within
// kDefaultAdvertiseTimeout (5 s), and each discovery attempt waits up to
// kDefaultDiscoveryTimeout (5 s); 60 s also covers launchd restarting a job
// that failed its first start (ThrottleInterval 10 s) a few times.
constexpr std::chrono::seconds kServiceStartTimeout{60};

struct Plan {
  WorkerServiceMode mode = WorkerServiceMode::user_agent;
  WorkerProbe probe;
  svp::exec::remote::PairingKey key;
  WorkerLayout layout;
  std::filesystem::path plist_path;
  std::string plist;
  std::string worker_record;
  // --system-service only: the worker user's staging directory.
  std::filesystem::path staging;
};

std::filesystem::path make_temporary_directory(const std::string& prefix) {
  std::string pattern = (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX")).string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw WorkerError(WorkerErrorCode::io, "cannot create a temporary directory");
  }
  return pattern;
}

void check_worker(const WorkerProbe& probe, const HostFacts& host, WorkerServiceMode mode,
                  std::uint64_t bytes_needed) {
  if (probe.arch != "arm64" || probe.arch != host.arch) {
    throw WorkerError(WorkerErrorCode::refused,
                      "the worker is " + probe.arch + "; workers must be Apple Silicon (arm64)");
  }
  if (probe.product_version != host.os.product_version) {
    throw WorkerError(WorkerErrorCode::refused,
                      "the worker runs macOS " + probe.product_version + " (" + probe.build +
                          ") but this Mac runs macOS " + host.os.product_version + " (" +
                          host.os.build + "); a worker must run the coordinator's macOS version");
  }
  const std::uint64_t available = mode == WorkerServiceMode::system_daemon
                                      ? probe.library_available_bytes
                                      : probe.home_available_bytes;
  if (available < bytes_needed) {
    throw WorkerError(WorkerErrorCode::refused,
                      "the worker has " + format_bytes(available) + " free; pairing needs " +
                          format_bytes(bytes_needed));
  }
  if (mode == WorkerServiceMode::user_agent &&
      (probe.system_daemon_plist_present || probe.system_daemon_pairings > 0)) {
    throw WorkerError(WorkerErrorCode::refused,
                      "the worker already runs SVP as a LaunchDaemon; pair with "
                      "--system-service or unpair that pairing first");
  }
  if (mode == WorkerServiceMode::system_daemon &&
      (probe.user_agent_job_loaded || probe.user_agent_pairings > 0)) {
    throw WorkerError(WorkerErrorCode::refused,
                      "the worker already runs SVP as a LaunchAgent; pair without "
                      "--system-service or unpair that pairing first");
  }
}

std::uint64_t runtime_bytes(const CoordinatorRuntime& runtime) {
  std::uint64_t total = runtime.manifest_bytes.size();
  for (const RuntimeFileSource& file : runtime.files) {
    total += file.blob.ref.bytes;
  }
  return total;
}

std::uint64_t model_bytes(const std::vector<ModelBundleSource>& bundles) {
  std::uint64_t total = 0;
  for (const ModelBundleSource& bundle : bundles) {
    total += bundle.manifest.ref.bytes;
    for (const BlobSource& file : bundle.files) {
      total += file.ref.bytes;
    }
  }
  return total;
}

std::vector<std::string> tar_command(const std::filesystem::path& directory) {
  return {std::string(kSystemTar), "-cf", "-", "-C", directory.string(), "."};
}

int dry_run(const Plan& plan, const CoordinatorRuntime& runtime,
            const std::filesystem::path& staged_runtime, const std::string& requested_dir) {
  const std::filesystem::path out = requested_dir.empty()
                                        ? make_temporary_directory("svp-pair-dry-run")
                                        : std::filesystem::path(requested_dir);
  std::filesystem::create_directories(out);
  const auto write = [&](const std::string& name, const std::string& text) {
    std::ofstream file(out / name, std::ios::binary | std::ios::trunc);
    file << text;
    return out / name;
  };
  const std::filesystem::path plist_file = write(std::string(kWorkerJobLabel) + ".plist", plan.plist);
  std::vector<std::filesystem::path> scripts;
  if (plan.mode == WorkerServiceMode::user_agent) {
    scripts.push_back(write("1-prepare-root.sh", render_prepare_root_script(plan.layout.root)));
    scripts.push_back(write("2-receive-runtime.sh",
                            render_receive_runtime_script(plan.layout.runtimes(), runtime.runtime_id)));
    scripts.push_back(write("3-write-pairing.sh",
                            render_write_file_script(plan.layout.pairings() /
                                                         (plan.key.pairing_id + ".json"),
                                                     0600)));
    scripts.push_back(write("4-point-current.sh",
                            render_point_current_script(plan.layout.root, runtime.runtime_id)));
    scripts.push_back(write("5-prepare-launch-agents.sh",
                            render_prepare_launch_agents_script(plan.plist_path, plan.layout.root)));
    scripts.push_back(write("6-write-plist.sh", render_write_file_script(plan.plist_path, 0644)));
    scripts.push_back(write("7-start-agent.sh",
                            render_agent_start_script(plan.plist_path)));
  } else {
    const WorkerLayout staging{.root = plan.staging};
    scripts.push_back(write("1-prepare-staging.sh", render_prepare_root_script(plan.staging)));
    scripts.push_back(write("2-receive-runtime.sh",
                            render_receive_runtime_script(staging.runtimes(), runtime.runtime_id)));
    scripts.push_back(write("3-write-pairing.sh",
                            render_write_file_script(staging.pairings() /
                                                         (plan.key.pairing_id + ".json"),
                                                     0600)));
    scripts.push_back(write("4-write-plist.sh",
                            render_write_file_script(
                                plan.staging / (std::string(kWorkerJobLabel) + ".plist"), 0644)));
    scripts.push_back(write("5-install-daemon.sh",
                            render_daemon_install_script(DaemonInstall{
                                .user = plan.probe.user,
                                .staging = plan.staging,
                                .root = plan.layout.root,
                                .plist = plan.plist_path,
                                .runtime_id = runtime.runtime_id})));
  }
  scripts.push_back(write("unpair.sh", render_removal_script(WorkerRemoval{
                                           .mode = plan.mode,
                                           .root = plan.layout.root,
                                           .plist = plan.plist_path,
                                           .pairing_id = plan.key.pairing_id})));
  std::filesystem::copy(staged_runtime, out / "runtime", std::filesystem::copy_options::recursive);

  int failures = 0;
  const std::string lint = "/usr/bin/plutil -lint " + shell_quote(plist_file.string());
  std::cout << "$ " << lint << "\n" << std::flush;
  failures += std::system(lint.c_str()) == 0 ? 0 : 1;
  for (const std::filesystem::path& script : scripts) {
    const std::string check = "/bin/sh -n " + shell_quote(script.string());
    const int status = std::system(check.c_str());
    std::cout << "sh -n " << script.filename().string() << ": " << (status == 0 ? "ok" : "FAILED")
              << "\n";
    failures += status == 0 ? 0 : 1;
  }
  const std::string verify = shell_quote((out / "runtime" / std::string(kSessionProgram)).string()) +
                             " worker verify-runtime --runtime-dir " +
                             shell_quote((out / "runtime").string()) + " --expect " +
                             svp::exec::blake3_prefixed(runtime.runtime_id);
  std::cout << "$ " << verify << "\n" << std::flush;
  failures += std::system(verify.c_str()) == 0 ? 0 : 1;
  std::cout << "dry run: nothing was changed on the worker; files in " << out.string() << "\n"
            << "  the real pairing runs the numbered scripts over ssh in order"
            << (plan.mode == WorkerServiceMode::system_daemon
                    ? ", the last one staged as " + (plan.staging / "install-daemon.sh").string() +
                          " and run with `ssh -t <worker> sudo /bin/sh <it>`"
                    : "")
            << "\n";
  return failures == 0 ? 0 : 1;
}

void install_user_agent(SshSession& ssh, const Plan& plan, const CoordinatorRuntime& runtime,
                        const std::filesystem::path& staged_runtime) {
  (void)ssh.run_script(render_prepare_root_script(plan.layout.root));
  std::cout << "copying runtime " << svp::exec::blake3_prefixed(runtime.runtime_id) << " ("
            << format_bytes(runtime_bytes(runtime)) << ") and verifying it on the worker\n";
  std::cout << "  "
            << ssh.run_script_fed_by(
                   render_receive_runtime_script(plan.layout.runtimes(), runtime.runtime_id),
                   tar_command(staged_runtime));
  (void)ssh.run_script(
      render_write_file_script(plan.layout.pairings() / (plan.key.pairing_id + ".json"), 0600),
      plan.worker_record);
  (void)ssh.run_script(render_point_current_script(plan.layout.root, runtime.runtime_id));
  (void)ssh.run_script(render_prepare_launch_agents_script(plan.plist_path, plan.layout.root));
  (void)ssh.run_script(render_write_file_script(plan.plist_path, 0644), plan.plist);
  std::cout << "loading LaunchAgent " << plan.plist_path.string() << "\n";
  (void)ssh.run_script(render_agent_start_script(plan.plist_path));
}

void install_system_daemon(SshSession& ssh, const Plan& plan, const CoordinatorRuntime& runtime,
                           const std::filesystem::path& staged_runtime) {
  const WorkerLayout staging{.root = plan.staging};
  (void)ssh.run_script(render_prepare_root_script(plan.staging));
  std::cout << "copying runtime " << svp::exec::blake3_prefixed(runtime.runtime_id) << " ("
            << format_bytes(runtime_bytes(runtime)) << ") and verifying it on the worker\n";
  std::cout << "  "
            << ssh.run_script_fed_by(
                   render_receive_runtime_script(staging.runtimes(), runtime.runtime_id),
                   tar_command(staged_runtime));
  (void)ssh.run_script(
      render_write_file_script(staging.pairings() / (plan.key.pairing_id + ".json"), 0600),
      plan.worker_record);
  (void)ssh.run_script(
      render_write_file_script(plan.staging / (std::string(kWorkerJobLabel) + ".plist"), 0644),
      plan.plist);
  const std::filesystem::path script = plan.staging / "install-daemon.sh";
  (void)ssh.run_script(render_write_file_script(script, 0700),
                       render_daemon_install_script(DaemonInstall{.user = plan.probe.user,
                                                                  .staging = plan.staging,
                                                                  .root = plan.layout.root,
                                                                  .plist = plan.plist_path,
                                                                  .runtime_id = runtime.runtime_id}));
  std::cout << "installing LaunchDaemon " << plan.plist_path.string()
            << " (sudo asks for the worker's administrator password)\n"
            << std::flush;
  ssh.run_interactive("sudo /bin/sh " + shell_quote(script.string()));
}

void roll_back(SshSession& ssh, const Plan& plan) {
  const std::string script = render_removal_script(WorkerRemoval{.mode = plan.mode,
                                                                 .root = plan.layout.root,
                                                                 .plist = plan.plist_path,
                                                                 .pairing_id = plan.key.pairing_id});
  try {
    if (plan.mode == WorkerServiceMode::user_agent) {
      (void)ssh.run_script(script);
    } else {
      (void)ssh.run_script("rm -rf " + shell_quote(plan.staging.string()));
      ssh.run_interactive("sudo /bin/sh -c " + shell_quote(script));
    }
    std::cerr << "svp-builder: rolled the worker back\n";
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: could not roll the worker back (" << error.what()
              << "); remove it with `svp-builder workers unpair` after fixing the problem\n";
  }
}

}  // namespace

int run_workers_pair(const WorkersCliOptions& options) {
  // tar must not add AppleDouble files for extended attributes.
  ::setenv("COPYFILE_DISABLE", "1", 1);
  const CoordinatorContext context = load_coordinator_context();
  const HostFacts host = detect_host_facts();
  const CoordinatorRuntime& runtime = context.runtime;
  std::cout << "coordinator: macOS " << host.os.product_version << " (" << host.os.build << "), runtime "
            << svp::exec::blake3_prefixed(runtime.runtime_id) << " ("
            << runtime_kind_name(runtime.kind) << ", " << runtime.files.size() << " file(s), "
            << format_bytes(runtime_bytes(runtime)) << ")\n";
  if (!runtime.fallback_reason.empty()) {
    std::cout << "  note: " << runtime.fallback_reason << "\n";
  }
  const std::vector<ModelBundleSource> models = select_model_bundles(context, options.models);

  Plan plan;
  plan.mode = options.system_service ? WorkerServiceMode::system_daemon
                                     : WorkerServiceMode::user_agent;
  SshSession ssh(SshTarget{.destination = options.pair_target, .options = options.ssh_options});
  plan.probe = parse_probe_output(ssh.run_script(render_probe_script()));
  std::cout << "worker: " << plan.probe.user << " (uid " << plan.probe.uid << "), macOS "
            << plan.probe.product_version << " (" << plan.probe.build << ") " << plan.probe.arch
            << ", " << format_bytes(plan.mode == WorkerServiceMode::system_daemon
                                        ? plan.probe.library_available_bytes
                                        : plan.probe.home_available_bytes)
            << " free\n";
  check_worker(plan.probe, host, plan.mode,
               runtime_bytes(runtime) + model_bytes(models) + kPairingDiskHeadroomBytes);

  plan.key = generate_pairing_key();
  plan.layout = WorkerLayout{.root = default_worker_root(plan.mode, plan.probe.home)};
  plan.plist_path = launchd_plist_path(plan.mode, plan.probe.home);
  plan.plist = render_launchd_plist(make_worker_service_spec(
      plan.mode, plan.layout, plan.probe.user, plan.probe.home, plan.probe.login_path));
  plan.worker_record = encode_worker_pairing(
      WorkerPairingRecord{.key = plan.key, .created_at = utc_timestamp_now()});
  plan.staging = std::filesystem::path(plan.probe.home) / "Library" / "Caches" / "org.svp" /
                 ("worker-staging-" + plan.key.pairing_id);

  const std::filesystem::path local = make_temporary_directory("svp-pair");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{local};
  const std::filesystem::path staged_runtime = local / "runtime";
  stage_runtime_directory(runtime, staged_runtime);

  if (options.dry_run) {
    return dry_run(plan, runtime, staged_runtime, options.dry_run_dir);
  }

  try {
    if (plan.mode == WorkerServiceMode::user_agent) {
      install_user_agent(ssh, plan, runtime, staged_runtime);
    } else {
      install_system_daemon(ssh, plan, runtime, staged_runtime);
    }
  } catch (const std::exception&) {
    roll_back(ssh, plan);
    throw;
  }

  CoordinatorPairingRecord record;
  record.key = plan.key;
  record.created_at = utc_timestamp_now();
  record.runtime_id = runtime.runtime_id;
  record.runtime_kind = runtime.kind;
  record.worker = WorkerEndpoint{.ssh_target = options.pair_target,
                                 .user = plan.probe.user,
                                 .uid = plan.probe.uid,
                                 .home = plan.probe.home,
                                 .root = plan.layout.root.string(),
                                 .service_mode = plan.mode,
                                 .label = std::string(kWorkerJobLabel),
                                 .plist = plan.plist_path.string(),
                                 .arch = plan.probe.arch,
                                 .os = OsIdentity{.product_version = plan.probe.product_version,
                                                  .build = plan.probe.build}};
  const PairingDirectory store(default_coordinator_pairings_dir());
  store.write(record.key.pairing_id, encode_coordinator_pairing(record));
  std::cout << "paired " << options.pair_target << " as " << record.key.pairing_id << " ("
            << worker_service_mode_name(plan.mode) << "); secret stored in "
            << store.file_for(record.key.pairing_id).string() << "\n";

  const WorkerReach reach = wait_until_reachable(record, context, kServiceStartTimeout);
  if (!reach.reachable) {
    std::cerr << "svp-builder: the worker job is installed but did not answer: " << reach.error
              << "\n  see " << (plan.layout.agent_log()).string()
              << " on the worker; `svp-builder workers unpair " << record.key.pairing_id
              << "` removes it\n";
    return 1;
  }
  std::cout << "worker reachable by pairing id via " << reach.route->route.interface_name << " ("
            << svp::exec::remote::route_medium_name(reach.route->route.medium) << ") as `"
            << reach.route->service_name << "`; runtime present: "
            << (reach.ack->runtime_present ? "yes" : "NO") << "\n";

  if (!models.empty()) {
    const std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
    WorkerSessionClient client(*connection->reader, *connection->writer);
    (void)client.hello(context.hello());
    TransferStats stats;
    client.ensure_model_bundles(models, stats);
    client.shutdown();
    std::cout << "model bundles: " << stats.model_bundles_pushed.size() << " pushed and verified ("
              << format_bytes(stats.bytes_sent) << "), "
              << models.size() - stats.model_bundles_pushed.size() << " already present\n";
  }
  return calibrate_for_workers_command(record, context.hello(), context.runtime,
                                       context.model_cache)
             ? 0
             : 1;
}

}  // namespace svp::builder::workers
