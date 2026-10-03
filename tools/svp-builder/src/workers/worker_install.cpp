// `svp-builder worker install --join <worker token>`: run ON a new worker Mac
// that has nothing from SVP but this svp-builder (and its runtime bundle).
// No coordinator SSH key: the Mac's administrator types the password once,
// for sudo, and from then on coordinators of the fleet pair it
// (fleet_join.hpp) and update it (service_updater.hpp) without anyone.
//
//   1. Check the token (a worker token, not expired) and this Mac (no SVP
//      LaunchAgent already, which would compete with the daemon).
//   2. Stage, in the user's ~/Library/Caches/org.svp/worker-staging-<id>:
//      the runtime this svp-builder belongs to (runtimes/<hex>, verified
//      here), join.json (the credential, 0600), the LaunchDaemon plist
//      (<root>/current/bin/svp-builder worker serve --root <root>), and the
//      install script (worker_scripts.hpp render_daemon_install_script).
//   3. `sudo /bin/sh <script>`: moves them into
//      /Library/Application Support/SVP/Worker owned by this user, points
//      <root>/current at the runtime, installs the plist, and starts the job.
// Re-running it on an installed Mac keeps its join id (and member key) when
// the token is of the same fleet, so coordinators do not pair it twice.

#include "coordinator_context.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/join_service.hpp"
#include "svp/exec/worker/launchd_job.hpp"
#include "svp/exec/worker/runtime_source.hpp"
#include "svp/exec/worker/runtime_store.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_scripts.hpp"
#include "workers_cli.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;

constexpr std::string_view kSystemSudo = "/usr/bin/sudo";

std::string run_local_script(const std::string& script) {
  const std::string command = "/bin/sh -c " + shell_quote(script);
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    throw WorkerError(WorkerErrorCode::command, "cannot run /bin/sh");
  }
  std::string output;
  std::array<char, 4096> buffer{};
  while (const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
    output.append(buffer.data(), count);
  }
  const int status = ::pclose(pipe);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw WorkerError(WorkerErrorCode::command, "probing this Mac failed");
  }
  return output;
}

void write_file(const std::filesystem::path& path, const std::string& text,
                std::filesystem::perms mode) {
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) {
      throw WorkerError(WorkerErrorCode::io, "cannot write " + path.string());
    }
  }
  std::filesystem::permissions(path, mode, std::filesystem::perm_options::replace);
}

std::filesystem::path make_temporary_directory() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "svp-worker-install-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw WorkerError(WorkerErrorCode::io, "cannot create a temporary directory");
  }
  return pattern;
}

}  // namespace

int run_worker_install(const WorkerCliOptions& options) {
  const WorkerJoinToken token = decode_worker_token(read_token_argument(options.join_token));
  if (utc_seconds_now() > token.expires_at) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "this worker token has expired; ask a coordinator for a new one "
                      "(`svp-builder workers fleet token`)");
  }

  const WorkerProbe probe = parse_probe_output(run_local_script(render_probe_script()));
  const HostFacts host = detect_host_facts();
  if (probe.arch != "arm64" || host.arch != "arm64") {
    throw WorkerError(WorkerErrorCode::refused, "workers must be Apple Silicon (arm64)");
  }
  if (probe.user_agent_job_loaded || probe.user_agent_pairings > 0) {
    throw WorkerError(WorkerErrorCode::refused,
                      "this Mac already runs SVP as a LaunchAgent; unpair it first");
  }

  const WorkerLayout layout{.root = default_worker_root(WorkerServiceMode::system_daemon,
                                                        probe.home)};
  // A Mac installed again for the same fleet keeps its worker key, join id,
  // and member key, so its coordinators do not pair it twice; one installed
  // before worker keys existed gets a key-bound id now (fleet_store.hpp).
  WorkerJoinCredential credential = new_worker_join_credential(token);
  try {
    if (std::optional<WorkerJoinCredential> existing = load_worker_join_credential(layout);
        existing && existing->token.fleet_id == token.fleet_id &&
        !migrate_join_credential(*existing)) {
      credential.worker_key = existing->worker_key;
      credential.worker_join_id = existing->worker_join_id;
      credential.member_key = existing->member_key;
    }
  } catch (const std::exception&) {
    // An unreadable earlier credential is replaced.
  }

  const CoordinatorRuntime runtime = locate_coordinator_runtime(current_executable());
  const std::filesystem::path staging =
      options.dry_run
          ? (options.dry_run_dir.empty() ? make_temporary_directory()
                                         : std::filesystem::path(options.dry_run_dir)) /
                "staging"
          : std::filesystem::path(probe.home) / "Library" / "Caches" / "org.svp" /
                ("worker-staging-" + credential.worker_join_id);
  std::error_code ignored;
  std::filesystem::remove_all(staging, ignored);
  const WorkerLayout staged{.root = staging};
  create_worker_layout(staged);
  stage_runtime_directory(runtime, staged.runtime(runtime.runtime_id));
  (void)verify_runtime_directory(staged.runtime(runtime.runtime_id), runtime.runtime_id);
  save_worker_join_credential(staged, credential);
  const std::filesystem::path plist_path =
      launchd_plist_path(WorkerServiceMode::system_daemon, probe.home);
  write_file(staging / (std::string(kWorkerJobLabel) + ".plist"),
             render_launchd_plist(make_worker_service_spec(WorkerServiceMode::system_daemon,
                                                           layout, probe.user, probe.home,
                                                           probe.login_path)),
             std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                 std::filesystem::perms::group_read | std::filesystem::perms::others_read);
  const std::filesystem::path script = staging / "install-daemon.sh";
  write_file(script,
             render_daemon_install_script(DaemonInstall{.user = probe.user,
                                                        .staging = staging,
                                                        .root = layout.root,
                                                        .plist = plist_path,
                                                        .runtime_id = runtime.runtime_id}),
             std::filesystem::perms::owner_all);

  const std::string command =
      std::string(kSystemSudo) + " /bin/sh " + shell_quote(script.string());
  std::cout << "worker " << credential.worker_join_id << " of fleet " << token.fleet_id
            << ": runtime " << svp::exec::blake3_prefixed(runtime.runtime_id) << " ("
            << runtime_kind_name(runtime.kind) << "), service "
            << layout.service_program().string() << "\n";
  if (options.dry_run) {
    std::cout << "dry run: staged in " << staging.string() << "; the install runs\n  "
              << command << "\n";
    return 0;
  }
  std::cout << "installing LaunchDaemon " << plist_path.string()
            << " (sudo asks for this Mac's administrator password)\n"
            << std::flush;
  const int status = std::system(command.c_str());
  if (status != 0) {
    throw WorkerError(WorkerErrorCode::command,
                      "the install script failed; staged files are in " + staging.string());
  }
  std::cout << "installed; coordinators of fleet " << token.fleet_id
            << " pair this Mac on their next `workers fleet pair` or `build --distributed`\n";
  return 0;
}

}  // namespace svp::builder::workers
