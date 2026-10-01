#pragma once

// Test-only: runs `svp-exec-test-worker --listen` as a child process and
// waits until it is discoverable.

#include "pairing_test_support.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace svp::exec::remote::test {

// How long a worker may take to bind and register its Bonjour name; above
// kDefaultAdvertiseTimeout so the worker reports its own failure first.
inline constexpr std::chrono::milliseconds kWorkerStartLimit{15'000};

class ListeningWorkerProcess {
 public:
  ListeningWorkerProcess(std::filesystem::path executable, PairingKey key,
                         std::filesystem::path directory)
      : executable_(std::move(executable)),
        key_(std::move(key)),
        secret_file_(directory / (key_.pairing_id + ".psk")) {
    write_pairing_file(secret_file_.string(), key_);
    start();
  }
  ~ListeningWorkerProcess() { kill(); }
  ListeningWorkerProcess(const ListeningWorkerProcess&) = delete;
  ListeningWorkerProcess& operator=(const ListeningWorkerProcess&) = delete;

  [[nodiscard]] const PairingKey& key() const { return key_; }
  [[nodiscard]] pid_t pid() const { return pid_; }
  // The "listening service=<name> port=<n>" line the worker printed.
  [[nodiscard]] const std::string& banner() const { return banner_; }

  // Spawns the worker and blocks until it announces it is listening.
  void start() {
    if (pid_ > 0) {
      throw std::runtime_error("worker already running");
    }
    int out[2] = {-1, -1};
    if (::pipe(out) != 0) {
      throw std::runtime_error("pipe failed");
    }
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    // The worker takes SIGINT/SIGTERM with sigwait; start it with them
    // unblocked and at their defaults whatever this process did.
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attributes, &none);

    const std::string program = executable_.string();
    std::vector<std::string> arguments{program,         "--listen",
                                       "--psk-file",    secret_file_.string(),
                                       "--pairing-id",  key_.pairing_id,
                                       "--service-name", key_.pairing_id};
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const int status =
        ::posix_spawn(&pid_, program.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(out[1]);
    if (status != 0) {
      ::close(out[0]);
      pid_ = -1;
      throw std::runtime_error("cannot spawn test worker: " + std::to_string(status));
    }
    banner_ = read_line(out[0]);
    ::close(out[0]);
    if (!banner_.starts_with("listening ")) {
      kill();
      throw std::runtime_error("test worker did not start listening: `" + banner_ + "`");
    }
  }

  // SIGKILL and reap; the worker gets no chance to say goodbye.
  void kill() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
    }
  }

  // True once the process has exited on its own (reaps it).
  bool exited() {
    if (pid_ <= 0) {
      return true;
    }
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      return true;
    }
    return false;
  }

 private:
  static std::string read_line(int fd) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + kWorkerStartLimit;
    while (true) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) {
        return line;
      }
      pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};
      if (::poll(&poll_fd, 1, static_cast<int>(left.count())) <= 0) {
        continue;
      }
      char character = 0;
      if (::read(fd, &character, 1) != 1 || character == '\n') {
        return line;
      }
      line += character;
    }
  }

  std::filesystem::path executable_;
  PairingKey key_;
  std::filesystem::path secret_file_;
  pid_t pid_ = -1;
  std::string banner_;
};

}  // namespace svp::exec::remote::test
