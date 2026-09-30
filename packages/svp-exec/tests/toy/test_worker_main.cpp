// svp-exec-test-worker: serves run_worker_loop on stdin/stdout with the toy
// task type and its faults honoured. The loop writes into an internal pipe;
// a relay copies frames to stdout and corrupts the last payload byte of any
// RESULT whose diagnostics carry kToyCorruptInTransit, so the coordinator
// sees a payload hash mismatch exactly as it would from a faulty link.

#include "in_memory_artifact_store.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/worker_loop.hpp"
#include "toy_tasks.hpp"

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace svp::exec;

bool write_all(int fd, const std::vector<std::byte>& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool should_corrupt(const Frame& frame) {
  if (frame.type != MessageType::result || frame.payloads.empty()) {
    return false;
  }
  const nlohmann::json& diagnostics = frame.body.at("task_result").at("diagnostics");
  return diagnostics.contains(std::string(test::kToyCorruptInTransit));
}

void relay_frames(int from_fd, int to_fd) {
  FdFrameReader reader(from_fd);
  try {
    while (auto frame = reader.read()) {
      std::vector<std::byte> bytes = encode_frame(*frame);
      if (should_corrupt(*frame)) {
        bytes.back() ^= std::byte{0x01};
      }
      if (!write_all(to_fd, bytes)) {
        return;
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "svp-exec-test-worker relay: %s\n", error.what());
  }
}

}  // namespace

int main() {
  // The coordinator may vanish mid-write; report it as a write error.
  std::signal(SIGPIPE, SIG_IGN);

  test::InMemoryArtifactStore store;
  TaskTypeRegistry registry;
  test::register_toy_tasks(registry, store,
                           test::ToyTaskOptions{.faults = test::ToyFaults::honoured});

  int relay[2] = {-1, -1};
  if (::pipe(relay) != 0) {
    std::perror("svp-exec-test-worker: pipe");
    return 1;
  }
  std::thread relay_thread([&] { relay_frames(relay[0], STDOUT_FILENO); });

  const WorkerLoopExit exit = run_worker_loop(
      STDIN_FILENO, relay[1], registry, store,
      WorkerLoopOptions{.worker_session_id = "ws_test_worker_" + std::to_string(::getpid())});
  ::close(relay[1]);
  relay_thread.join();
  ::close(relay[0]);
  return exit == WorkerLoopExit::protocol_error ? 1 : 0;
}
