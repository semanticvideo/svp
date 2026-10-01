#pragma once

// The launchd job that runs the worker agent (plan §3.3). One job per worker
// Mac serves every pairing in its root; its program is the svp-builder of a
// verified runtime under that root, so nothing outside the root is executed.
//
//   RunAtLoad true, KeepAlive {SuccessfulExit: false}: started at load and
//     restarted after a crash, but an agent that exits cleanly (no pairings
//     left) stays down.
//   ThrottleInterval kWorkerJobThrottleSeconds: launchd's own default (10 s),
//     stated explicitly because RemoteExecutor's reconnect window
//     (remote_executor.hpp) is sized from it.
//   ProcessType Standard: Background would confine the agent's sessions to
//     efficiency cores and throttle their I/O on Apple Silicon, so measured
//     capacity would understate the Mac; admission (admission.hpp) is what
//     protects other workloads.
//   Umask 077: everything the agent creates is private to the worker's user.
//   LaunchDaemon only: UserName runs the job as the worker's user, never as
//     root (plan §4.3), and HOME is set to that user's home because a daemon
//     has no login session to inherit it from.

#include "svp/exec/worker/worker_layout.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::exec::worker {

inline constexpr int kWorkerJobThrottleSeconds = 10;
// Octal 077; the plist stores it as the integer 63.
inline constexpr int kWorkerJobUmask = 077;

struct WorkerServiceSpec {
  WorkerServiceMode mode = WorkerServiceMode::user_agent;
  std::string label = std::string(kWorkerJobLabel);
  // Absolute path of the program, then its arguments.
  std::vector<std::string> program_arguments;
  std::filesystem::path working_directory;
  std::filesystem::path log_path;
  // system_daemon only.
  std::string user_name;
  std::filesystem::path home;
  // The job's PATH; empty leaves launchd's default.
  std::string path;
};

// The job for a worker whose agent runs from the runtime `runtime_dir`:
// `<runtime_dir>/bin/svp-builder worker serve --root <root>`.
[[nodiscard]] WorkerServiceSpec make_worker_service_spec(WorkerServiceMode mode,
                                                         const WorkerLayout& layout,
                                                         const std::filesystem::path& runtime_dir,
                                                         const std::string& user_name,
                                                         const std::filesystem::path& home,
                                                         std::string path = {});

// XML property list (version 1.0). Throws WorkerError(configuration) for a
// spec missing its label, program, or (daemon) user.
[[nodiscard]] std::string render_launchd_plist(const WorkerServiceSpec& spec);

}  // namespace svp::exec::worker
