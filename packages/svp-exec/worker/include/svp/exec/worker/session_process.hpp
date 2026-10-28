#pragma once

// The process a worker agent starts for each coordinator session (plan
// §3.1, §4.3): the session program of the runtime the coordinator asked for,
// after that runtime verified, serving the svp-exec worker loop over one end
// of a socket pair on file descriptor kSessionFrameFd:
//
//   /usr/sbin/taskpolicy -c utility
//   <runtime>/bin/svp-builder worker --serve-fd 3
//       --cas-root <worker cache/cas> --session-dir <session scratch>
//       --model-store <worker models> --worker-session-id <id>
//       --runtime-id b3:<hex>
//
// --model-store is the worker's content-addressed store of verified model
// bundles (model_bundles.hpp); task types that load models build their
// model cache from the bundles a task names.
//
// It is started through `/usr/sbin/taskpolicy -c utility`, which caps it at
// utility priority and then execs the session program (same pid). The agent
// runs uncapped so its connections keep up (launchd_job.hpp ProcessType);
// the tasks a session runs keep the cap launchd's Standard gave the whole
// job before, so they leave foreground work on the worker Mac ahead of them,
// on the CPU and on disk.
//
// stdin is /dev/null and stdout/stderr are the agent's (its log), so nothing
// a task prints can corrupt the frame stream. Every path here is the
// worker's own; none comes from the coordinator.

#include "svp/exec/blake3_digest.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace svp::exec::worker {

inline constexpr int kSessionFrameFd = 3;
// Runs a program under a QoS clamp (taskpolicy(8)), keeping its pid.
inline constexpr std::string_view kSessionCapProgram = "/usr/sbin/taskpolicy";
inline constexpr std::string_view kSessionCapClamp = "utility";

struct SessionLaunch {
  std::filesystem::path program;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
};

[[nodiscard]] SessionLaunch make_session_launch(const std::filesystem::path& runtime_dir,
                                                const std::filesystem::path& cas_root,
                                                const std::filesystem::path& session_dir,
                                                const std::filesystem::path& model_store,
                                                const std::string& worker_session_id,
                                                const Blake3Digest& runtime_id);

struct SessionProcess {
  pid_t pid = -1;
  // The agent's end of the socket pair (close-on-exec, no SIGPIPE).
  int fd = -1;
};

// Throws WorkerError(io) when the process cannot be started.
[[nodiscard]] SessionProcess spawn_session_process(const SessionLaunch& launch);

// Closes the agent's end, gives the process `grace` to exit on its own, then
// SIGKILLs it, and reaps it. Returns the wait status.
int finish_session_process(SessionProcess& process, std::chrono::milliseconds grace);

using SessionLauncher = std::function<SessionProcess(const SessionLaunch&)>;

}  // namespace svp::exec::worker
