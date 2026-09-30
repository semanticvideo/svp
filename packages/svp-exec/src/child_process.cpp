#include "child_process.hpp"

#include "svp/exec/exec_error.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace svp::exec::detail {
namespace {

[[noreturn]] void throw_os(std::string_view what, int error) {
  throw ExecError(ExecErrorCode::invalid_value,
                  std::string(what) + ": " + std::strerror(error));
}

void make_socket_pair(int (&fds)[2]) {
#ifdef SOCK_CLOEXEC
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
    throw_os("socketpair failed", errno);
  }
#else
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw_os("socketpair failed", errno);
  }
  for (const int fd : fds) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
#endif
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  ::setsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

// RAII for the spawn attribute objects.
struct SpawnSetup {
  posix_spawn_file_actions_t actions{};
  posix_spawnattr_t attributes{};

  SpawnSetup() {
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
  }
  ~SpawnSetup() {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
  }
  SpawnSetup(const SpawnSetup&) = delete;
  SpawnSetup& operator=(const SpawnSetup&) = delete;
};

}  // namespace

ChildProcess spawn_child_process(const std::filesystem::path& executable,
                                 const std::vector<std::string>& arguments) {
  int fds[2] = {-1, -1};
  make_socket_pair(fds);
  const int parent_fd = fds[0];
  const int child_fd = fds[1];

  SpawnSetup setup;
  posix_spawn_file_actions_adddup2(&setup.actions, child_fd, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&setup.actions, child_fd, STDOUT_FILENO);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  // Apple: close every descriptor not named in the file actions, so
  // concurrent spawns never leak each other's sockets (which would hide a
  // child's death from its reader). stderr stays with the parent.
  posix_spawn_file_actions_addinherit_np(&setup.actions, STDERR_FILENO);
  posix_spawnattr_setflags(&setup.attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

  const std::string program = executable.string();
  std::vector<std::string> argv_storage;
  argv_storage.push_back(program);
  argv_storage.insert(argv_storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (std::string& argument : argv_storage) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int status = ::posix_spawn(&pid, program.c_str(), &setup.actions,
                                   &setup.attributes, argv.data(), environ);
  ::close(child_fd);
  if (status != 0) {
    ::close(parent_fd);
    throw_os("cannot start worker `" + program + "`", status);
  }
  return ChildProcess{.pid = pid, .fd = parent_fd};
}

void kill_and_reap(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}

}  // namespace svp::exec::detail
