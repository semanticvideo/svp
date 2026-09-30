// svp-exec-test-worker: serves run_worker_loop on stdin/stdout with the toy
// task type and its faults honoured. By default toy outputs live in memory;
// with `--cas-root <dir>` inputs are resolved from, and outputs stored in, the
// content-addressed cache at <dir> through CasTaskArtifactAccess, as a real
// worker does. The loop writes into an internal pipe;
// a relay copies frames to stdout and corrupts the last payload byte of any
// RESULT whose diagnostics carry kToyCorruptInTransit, so the coordinator
// sees a payload hash mismatch exactly as it would from a faulty link.

#include "in_memory_artifact_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/worker_loop.hpp"
#include "toy_tasks.hpp"

#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

int main(int argc, char** argv) {
  // The coordinator may vanish mid-write; report it as a write error.
  std::signal(SIGPIPE, SIG_IGN);

  std::optional<std::string> cas_root;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--cas-root" && index + 1 < argc) {
      cas_root = argv[++index];
    } else {
      std::fprintf(stderr, "svp-exec-test-worker: unknown argument %s\n", argv[index]);
      return 2;
    }
  }
  const std::string session_id = "ws_test_worker_" + std::to_string(::getpid());
  const test::ToyTaskOptions toy_options{.faults = test::ToyFaults::honoured};

  test::InMemoryArtifactStore memory_store;
  std::unique_ptr<CasTaskArtifactAccess> cas_access;
  TaskArtifactAccess* artifacts = &memory_store;
  TaskTypeRegistry registry;
  if (cas_root) {
    CacheResult<CasStore> store = CasStore::at(*cas_root);
    if (!store) {
      std::fprintf(stderr, "svp-exec-test-worker: cache %s: %s\n", cas_root->c_str(),
                   store.error().message.c_str());
      return 2;
    }
    cas_access = std::make_unique<CasTaskArtifactAccess>(std::move(store).value(), session_id);
    artifacts = cas_access.get();
    test::register_toy_tasks(
        registry,
        [access = cas_access.get()](std::vector<std::byte> bytes, std::string media_type,
                                    std::string role) {
          return access->put(bytes, std::move(media_type), std::move(role));
        },
        toy_options);
  } else {
    test::register_toy_tasks(registry, memory_store, toy_options);
  }

  int relay[2] = {-1, -1};
  if (::pipe(relay) != 0) {
    std::perror("svp-exec-test-worker: pipe");
    return 1;
  }
  std::thread relay_thread([&] { relay_frames(relay[0], STDOUT_FILENO); });

  const WorkerLoopExit exit = run_worker_loop(
      STDIN_FILENO, relay[1], registry, *artifacts,
      WorkerLoopOptions{.worker_session_id = session_id});
  ::close(relay[1]);
  relay_thread.join();
  ::close(relay[0]);
  return exit == WorkerLoopExit::protocol_error ? 1 : 0;
}
