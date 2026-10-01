#pragma once

// `svp-builder workers ...` (coordinator side) and `svp-builder worker ...`
// (worker side) commands (plan §3.3, §5, §6). Apple only.
//
//   workers pair <user>@<host> [--system-service] [--models all|none|<ids>]
//                [--ssh-option <opt>]... [--dry-run [--dry-run-dir <dir>]]
//   workers list [--json]
//   workers status [<pairing-id>|<user>@<host>] [--json]
//   workers sync <pairing-id>|<user>@<host> [--models all|none|<ids>]
//   workers unpair <pairing-id>|<user>@<host> [--ssh-option <opt>]... [--forget]
//
//   worker serve --root <dir> [--memory-reserve-floor-mb <n>]
//       The launchd entry point (worker_agent.hpp).
//   worker --serve-fd <fd> --cas-root <dir> --session-dir <dir>
//          --worker-session-id <id> --runtime-id b3:<hex>
//       One session process (session_process.hpp).
//   worker verify-runtime --runtime-dir <dir> [--expect b3:<hex>]
//       Verifies a runtime directory against its manifest; prints its id.

#include <CLI/CLI.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::workers {

struct WorkersCliOptions {
  std::string pair_target;
  bool system_service = false;
  std::string models = "all";
  std::vector<std::string> ssh_options;
  bool dry_run = false;
  std::string dry_run_dir;
  std::string worker;
  bool json = false;
  bool forget = false;

  CLI::App* pair = nullptr;
  CLI::App* list = nullptr;
  CLI::App* status = nullptr;
  CLI::App* sync = nullptr;
  CLI::App* unpair = nullptr;
};

struct WorkerCliOptions {
  int serve_fd = -1;
  std::string cas_root;
  std::string session_dir;
  std::string worker_session_id;
  std::string runtime_id;
  std::string root;
  std::uint64_t memory_reserve_floor_mb = 0;
  std::string runtime_dir;
  std::string expect;

  CLI::App* worker = nullptr;
  CLI::App* serve = nullptr;
  CLI::App* verify_runtime = nullptr;
};

void register_workers_cli(CLI::App& app, WorkersCliOptions& workers, WorkerCliOptions& worker);

// The exit code of the selected workers/worker command, or nullopt when
// none was selected.
[[nodiscard]] std::optional<int> run_workers_cli(const WorkersCliOptions& workers,
                                                 const WorkerCliOptions& worker);

int run_workers_pair(const WorkersCliOptions& options);
int run_workers_list(const WorkersCliOptions& options);
int run_workers_status(const WorkersCliOptions& options);
int run_workers_sync(const WorkersCliOptions& options);
int run_workers_unpair(const WorkersCliOptions& options);

int run_worker_serve(const WorkerCliOptions& options);
int run_worker_session(const WorkerCliOptions& options);
int run_worker_verify_runtime(const WorkerCliOptions& options);

}  // namespace svp::builder::workers
