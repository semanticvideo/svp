#include "ssh_session.hpp"

#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_scripts.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace svp::builder::workers {
namespace {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

struct Spawned {
  pid_t pid = -1;
};

// stdin_fd / stdout_fd: -1 inherits this process's descriptor.
pid_t spawn(const std::vector<std::string>& arguments, int stdin_fd, int stdout_fd,
            const std::vector<int>& close_in_child) {
  posix_spawn_file_actions_t actions{};
  posix_spawn_file_actions_init(&actions);
  if (stdin_fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, stdin_fd, STDIN_FILENO);
  }
  if (stdout_fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);
  }
  for (const int fd : close_in_child) {
    if (fd >= 0 && fd != stdin_fd && fd != stdout_fd) {
      posix_spawn_file_actions_addclose(&actions, fd);
    }
  }
  std::vector<std::string> copy = arguments;
  std::vector<char*> argv;
  for (std::string& argument : copy) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t pid = -1;
  const int status = ::posix_spawn(&pid, copy.front().c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (status != 0) {
    throw WorkerError(WorkerErrorCode::command,
                      "cannot run " + copy.front() + ": " + std::strerror(status));
  }
  return pid;
}

int wait_for(pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

std::string read_all(int fd) {
  std::string output;
  char buffer[65536];
  while (true) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
    } else if (count == 0 || errno != EINTR) {
      return output;
    }
  }
}

void write_all(int fd, std::string_view bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return;  // The reader went away; its exit status reports why.
    }
    written += static_cast<std::size_t>(count);
  }
}

std::string remote_shell(const std::string& script) {
  return "/bin/sh -c " + svp::exec::worker::shell_quote(script);
}

}  // namespace

SshSession::SshSession(SshTarget target) : target_(std::move(target)) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "svp-ssh-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw WorkerError(WorkerErrorCode::io, "cannot create a private ssh control directory");
  }
  control_dir_ = pattern;
}

SshSession::~SshSession() {
  // Close the shared connection, if one was opened.
  std::vector<std::string> arguments = ssh_arguments(false);
  arguments.insert(arguments.end() - 1, {"-q", "-O", "exit"});
  const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  try {
    const pid_t pid = spawn(arguments, null_fd, null_fd, {});
    (void)wait_for(pid);
  } catch (const std::exception&) {
  }
  if (null_fd >= 0) {
    ::close(null_fd);
  }
  std::error_code error;
  std::filesystem::remove_all(control_dir_, error);
}

std::vector<std::string> SshSession::ssh_arguments(bool terminal) const {
  std::vector<std::string> arguments{std::string(kSystemSsh),
                                     "-o", "ControlMaster=auto",
                                     "-o", "ControlPath=" + (control_dir_ / "cm").string(),
                                     "-o", "ControlPersist=" + std::to_string(kSshControlPersistSeconds)};
  for (const std::string& option : target_.options) {
    arguments.push_back("-o");
    arguments.push_back(option);
  }
  if (terminal) {
    arguments.push_back("-t");
  }
  arguments.push_back(target_.destination);
  return arguments;
}

std::string SshSession::run_script(const std::string& script, std::string_view input) {
  std::vector<std::string> arguments = ssh_arguments(false);
  arguments.push_back(remote_shell(script));
  int in[2];
  int out[2];
  if (::pipe(in) != 0 || ::pipe(out) != 0) {
    throw WorkerError(WorkerErrorCode::io, "pipe failed");
  }
  const pid_t pid = spawn(arguments, in[0], out[1], {in[1], out[0]});
  ::close(in[0]);
  ::close(out[1]);
  std::thread feeder([fd = in[1], input] {
    write_all(fd, input);
    ::close(fd);
  });
  const std::string output = read_all(out[0]);
  ::close(out[0]);
  feeder.join();
  const int status = wait_for(pid);
  if (status != 0) {
    throw WorkerError(WorkerErrorCode::command, "ssh " + target_.destination +
                                                    " failed (exit " + std::to_string(status) +
                                                    "): " + output);
  }
  return output;
}

std::string SshSession::run_script_fed_by(const std::string& script,
                                          const std::vector<std::string>& local_program) {
  std::vector<std::string> arguments = ssh_arguments(false);
  arguments.push_back(remote_shell(script));
  int link[2];
  int out[2];
  if (::pipe(link) != 0 || ::pipe(out) != 0) {
    throw WorkerError(WorkerErrorCode::io, "pipe failed");
  }
  const pid_t producer = spawn(local_program, -1, link[1], {link[0], out[0], out[1]});
  ::close(link[1]);
  const pid_t consumer = spawn(arguments, link[0], out[1], {out[0]});
  ::close(link[0]);
  ::close(out[1]);
  const std::string output = read_all(out[0]);
  ::close(out[0]);
  const int producer_status = wait_for(producer);
  const int consumer_status = wait_for(consumer);
  if (producer_status != 0 || consumer_status != 0) {
    throw WorkerError(WorkerErrorCode::command,
                      local_program.front() + " | ssh " + target_.destination + " failed (exit " +
                          std::to_string(producer_status) + "/" + std::to_string(consumer_status) +
                          "): " + output);
  }
  return output;
}

void SshSession::run_interactive(const std::string& remote_command) {
  std::vector<std::string> arguments = ssh_arguments(true);
  arguments.push_back(remote_command);
  const pid_t pid = spawn(arguments, -1, -1, {});
  const int status = wait_for(pid);
  if (status != 0) {
    throw WorkerError(WorkerErrorCode::command, "ssh -t " + target_.destination + " `" +
                                                    remote_command + "` failed (exit " +
                                                    std::to_string(status) + ")");
  }
}

}  // namespace svp::builder::workers
