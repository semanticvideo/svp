#include "resize/cancellable_command.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace svp::builder::resize {
namespace {

std::string trimmed(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// The first line a failed command wrote to standard error, without ffmpeg's
// "[component @ 0x...]" prefixes: ffmpeg states its reason first and then
// how the failure spread.
std::string first_error_line(const std::filesystem::path& stderr_path) {
  std::ifstream input(stderr_path, std::ios::binary);
  for (std::string line; std::getline(input, line);) {
    std::string text = trimmed(line);
    while (text.starts_with('[')) {
      const std::size_t close = text.find("] ");
      if (close == std::string::npos) break;
      text = trimmed(text.substr(close + 2));
    }
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

std::string ending(int status) {
  if (WIFEXITED(status)) {
    return "exited with status " + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "ended by signal " + std::to_string(WTERMSIG(status));
  }
  return "ended with wait status " + std::to_string(status);
}

class SpawnSetup {
 public:
  SpawnSetup(const std::filesystem::path& stdout_path, const std::filesystem::path& stderr_path) {
    posix_spawn_file_actions_init(&actions_);
    posix_spawnattr_init(&attributes_);
    posix_spawn_file_actions_addopen(&actions_, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions_, STDOUT_FILENO, stdout_path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&actions_, STDERR_FILENO, stderr_path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    posix_spawnattr_setsigmask(&attributes_, &unblocked);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    posix_spawnattr_setsigdefault(&attributes_, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    // Only the three descriptors above reach the child (the builder's
    // progress stream stays private).
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes_, flags);
  }
  ~SpawnSetup() {
    posix_spawnattr_destroy(&attributes_);
    posix_spawn_file_actions_destroy(&actions_);
  }
  SpawnSetup(const SpawnSetup&) = delete;
  SpawnSetup& operator=(const SpawnSetup&) = delete;

  [[nodiscard]] const posix_spawn_file_actions_t* actions() const { return &actions_; }
  [[nodiscard]] const posix_spawnattr_t* attributes() const { return &attributes_; }

 private:
  posix_spawn_file_actions_t actions_{};
  posix_spawnattr_t attributes_{};
};

// Waits for `child`; true with its wait status, false on a wait error.
bool wait_for(pid_t child, int options, int& status, bool& exited) {
  while (true) {
    const pid_t waited = ::waitpid(child, &status, options);
    if (waited == child) {
      exited = true;
      return true;
    }
    if (waited == 0) {
      exited = false;
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

}  // namespace

CommandOutcome run_cancellable_command(const std::vector<std::string>& argv,
                                       const std::filesystem::path& stdout_path,
                                       const std::filesystem::path& stderr_path,
                                       const svp::exec::CancellationToken* cancellation) {
  CommandOutcome outcome;
  if (argv.empty()) {
    outcome.diagnostics = "no command";
    return outcome;
  }
  std::vector<char*> arguments;
  arguments.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    arguments.push_back(const_cast<char*>(argument.c_str()));
  }
  arguments.push_back(nullptr);

  const SpawnSetup setup(stdout_path, stderr_path);
  pid_t child = 0;
  const int spawned = ::posix_spawnp(&child, arguments[0], setup.actions(), setup.attributes(),
                                     arguments.data(), environ);
  if (spawned != 0) {
    outcome.diagnostics = "cannot run " + argv[0] + ": " + std::strerror(spawned);
    return outcome;
  }
  outcome.started = true;

  int status = 0;
  bool exited = false;
  bool stop_sent = false;
  while (true) {
    if (!wait_for(child, stop_sent ? 0 : WNOHANG, status, exited)) {
      outcome.diagnostics = "lost track of " + argv[0] + ": " + std::strerror(errno);
      return outcome;
    }
    if (exited) {
      break;
    }
    if (cancellation != nullptr && cancellation->requested()) {
      ::kill(child, SIGTERM);
      stop_sent = true;
      continue;
    }
    std::this_thread::sleep_for(kCommandCancellationPollInterval);
  }
  if (stop_sent) {
    outcome.cancelled = true;
    outcome.diagnostics = "stopped: the build was cancelled";
    return outcome;
  }
  outcome.succeeded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!outcome.succeeded) {
    const std::string errors = first_error_line(stderr_path);
    outcome.diagnostics = argv[0] + " " + ending(status) + (errors.empty() ? "" : ": " + errors);
  }
  return outcome;
}

}  // namespace svp::builder::resize
