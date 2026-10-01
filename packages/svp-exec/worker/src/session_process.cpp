#include "svp/exec/worker/session_process.hpp"

#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace svp::exec::worker {
namespace {

// How often finish_session_process looks for the process to have exited
// during its grace period; short against the grace periods used (seconds).
constexpr std::chrono::milliseconds kExitPollInterval{20};

}  // namespace

SessionLaunch make_session_launch(const std::filesystem::path& runtime_dir,
                                  const std::filesystem::path& cas_root,
                                  const std::filesystem::path& session_dir,
                                  const std::string& worker_session_id,
                                  const Blake3Digest& runtime_id) {
  return SessionLaunch{
      .program = runtime_dir / std::string(kSessionProgram),
      .arguments = {"worker", "--serve-fd", std::to_string(kSessionFrameFd), "--cas-root",
                    cas_root.string(), "--session-dir", session_dir.string(),
                    "--worker-session-id", worker_session_id, "--runtime-id",
                    blake3_prefixed(runtime_id)},
      .working_directory = session_dir};
}

SessionProcess spawn_session_process(const SessionLaunch& launch) {
  int pair[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
    throw WorkerError(WorkerErrorCode::io,
                      std::string("socketpair failed: ") + std::strerror(errno));
  }
  ::fcntl(pair[0], F_SETFD, FD_CLOEXEC);
  int no_sigpipe = 1;
  ::setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));

  posix_spawn_file_actions_t actions{};
  posix_spawnattr_t attributes{};
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attributes);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addinherit_np(&actions, STDOUT_FILENO);
  posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pair[1], kSessionFrameFd);
  if (!launch.working_directory.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, launch.working_directory.c_str());
  }
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK |
                                            POSIX_SPAWN_SETSIGDEF);
  sigset_t none;
  sigemptyset(&none);
  posix_spawnattr_setsigmask(&attributes, &none);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  sigaddset(&defaults, SIGINT);
  sigaddset(&defaults, SIGTERM);
  posix_spawnattr_setsigdefault(&attributes, &defaults);

  const std::string program = launch.program.string();
  std::vector<std::string> arguments{program};
  arguments.insert(arguments.end(), launch.arguments.begin(), launch.arguments.end());
  std::vector<char*> argv;
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t pid = -1;
  const int status =
      ::posix_spawn(&pid, program.c_str(), &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  ::close(pair[1]);
  if (status != 0) {
    ::close(pair[0]);
    throw WorkerError(WorkerErrorCode::io,
                      "cannot start " + program + ": " + std::strerror(status));
  }
  return SessionProcess{.pid = pid, .fd = pair[0]};
}

int finish_session_process(SessionProcess& process, std::chrono::milliseconds grace) {
  if (process.fd >= 0) {
    ::shutdown(process.fd, SHUT_RDWR);
    ::close(process.fd);
    process.fd = -1;
  }
  int status = 0;
  if (process.pid <= 0) {
    return status;
  }
  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (true) {
    const pid_t done = ::waitpid(process.pid, &status, WNOHANG);
    if (done == process.pid || (done < 0 && errno != EINTR)) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(process.pid, SIGKILL);
      while (::waitpid(process.pid, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    std::this_thread::sleep_for(kExitPollInterval);
  }
  process.pid = -1;
  return status;
}

}  // namespace svp::exec::worker
