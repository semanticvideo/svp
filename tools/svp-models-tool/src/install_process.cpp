#include "install_process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>

extern char** environ;

namespace svp::models::tool {
namespace {

volatile sig_atomic_t active_child = 0;
volatile sig_atomic_t interrupted = 0;

void interrupt_child(int /*signal*/) {
  interrupted = 1;
  if (active_child > 0) kill(static_cast<pid_t>(active_child), SIGINT);
}

}  // namespace

ProcessResult run_process(
    const std::vector<std::string>& arguments,
    const std::function<void(const std::string&)>& line_callback) {
  if (arguments.empty()) throw std::invalid_argument("process arguments are empty");
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("could not create process pipe: " +
                             std::string(std::strerror(errno)));
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGINT);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  struct sigaction new_action {};
  struct sigaction old_action {};
  new_action.sa_handler = interrupt_child;
  sigemptyset(&new_action.sa_mask);
  sigaction(SIGINT, &new_action, &old_action);
  interrupted = 0;

  pid_t child = -1;
  const int spawn_error = posix_spawn(
      &child, arguments.front().c_str(), &actions, &attributes,
      argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(pipe_fds[1]);
  if (spawn_error != 0) {
    close(pipe_fds[0]);
    sigaction(SIGINT, &old_action, nullptr);
    throw std::runtime_error("could not start " + arguments.front() + ": " +
                             std::string(std::strerror(spawn_error)));
  }

  active_child = child;
  ProcessResult result;
  std::exception_ptr callback_failure;
  const auto dispatch_line = [&](const std::string& line) {
    if (!line_callback || callback_failure) return;
    try {
      line_callback(line);
    } catch (...) {
      callback_failure = std::current_exception();
      kill(child, SIGTERM);
    }
  };
  std::string pending;
  char buffer[8192];
  while (true) {
    const ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
    if (count > 0) {
      result.output.append(buffer, static_cast<std::size_t>(count));
      pending.append(buffer, static_cast<std::size_t>(count));
      std::size_t newline = 0;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        dispatch_line(line);
      }
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  close(pipe_fds[0]);
  if (!pending.empty()) dispatch_line(pending);

  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  active_child = 0;
  sigaction(SIGINT, &old_action, nullptr);
  if (callback_failure) std::rethrow_exception(callback_failure);
  if (interrupted) {
    result.exit_code = 130;
  } else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  } else {
    result.exit_code = 2;
  }
  return result;
}

}  // namespace svp::models::tool
