#include "svp/exec/worker/runtime_test_start.hpp"

#include "svp/exec/worker/worker_layout.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace svp::exec::worker {
namespace {

// How often the exit of a program whose output has ended is polled; short
// against the deadline (seconds).
constexpr std::chrono::milliseconds kExitPollInterval{20};

std::string describe_status(int status) {
  if (WIFEXITED(status)) {
    return "exited with status " + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "was killed by signal " + std::to_string(WTERMSIG(status));
  }
  return "ended with wait status " + std::to_string(status);
}

}  // namespace

std::chrono::milliseconds test_start_deadline(std::uint64_t runtime_bytes) {
  const std::uint64_t verify_ms = runtime_bytes * 1000 / kTestStartMinVerifyBytesPerSecond;
  return std::chrono::duration_cast<std::chrono::milliseconds>(kTestStartLaunchAllowance) +
         std::chrono::milliseconds(static_cast<std::int64_t>(verify_ms));
}

TestStartOutcome test_start_runtime(const std::filesystem::path& runtime_dir,
                                    const Blake3Digest& runtime_id,
                                    std::chrono::milliseconds deadline) {
  const std::string program = (runtime_dir / std::string(kSessionProgram)).string();
  const std::string expected = blake3_prefixed(runtime_id);
  std::vector<std::string> arguments = {program,         "worker", "verify-runtime",
                                        "--runtime-dir", runtime_dir.string(),
                                        "--expect",      expected};
  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {
    return {false, std::string("pipe failed: ") + std::strerror(errno)};
  }
  ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions{};
  posix_spawnattr_t attributes{};
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attributes);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
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
  sigaddset(&defaults, SIGUSR1);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  std::vector<char*> argv;
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t pid = -1;
  const int spawned =
      ::posix_spawn(&pid, program.c_str(), &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  ::close(pipe_fds[1]);
  if (spawned != 0) {
    ::close(pipe_fds[0]);
    return {false, "cannot start " + program + ": " + std::strerror(spawned)};
  }

  const auto end = std::chrono::steady_clock::now() + deadline;
  std::string output;
  bool timed_out = false;
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      timed_out = true;
      break;
    }
    pollfd entry{.fd = pipe_fds[0], .events = POLLIN, .revents = 0};
    const int ready = ::poll(&entry, 1, static_cast<int>(left.count()));
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0) {
      timed_out = ready == 0;
      break;
    }
    std::array<char, 4096> buffer{};
    const ssize_t count = ::read(pipe_fds[0], buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(pipe_fds[0]);

  int status = 0;
  while (!timed_out) {
    const pid_t done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      break;
    }
    if (done < 0 && errno != EINTR) {
      return {false, std::string("waitpid failed: ") + std::strerror(errno)};
    }
    if (std::chrono::steady_clock::now() >= end) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(kExitPollInterval);
  }
  if (timed_out) {
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return {false, program + " did not finish verifying within " +
                       std::to_string(deadline.count()) + " ms"};
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return {false, program + " " + describe_status(status)};
  }
  const std::string want = "runtime_id=" + expected;
  if (output.find(want) == std::string::npos) {
    return {false, program + " did not report " + want};
  }
  return {true, {}};
}

}  // namespace svp::exec::worker
