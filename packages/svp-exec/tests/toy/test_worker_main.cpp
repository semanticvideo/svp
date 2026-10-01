// svp-exec-test-worker: serves run_worker_loop with the toy task type and its
// faults honoured.
//
//   svp-exec-test-worker [--cas-root <dir>]
//       One session on stdin/stdout (the loopback executor's child).
//   svp-exec-test-worker --listen --psk-file <hex file> --pairing-id <id>
//                        [--service-name <bonjour name>] [--port <n>]
//                        [--cas-root <dir>]
//       Apple only. A RemoteListener: TLS 1.2 PSK, Bonjour _svp-worker._tcp
//       with the pairing id in TXT, one worker loop per connection. Prints
//       "listening service=<name> port=<n>" once discoverable and serves
//       until SIGINT or SIGTERM.
//
//   svp-exec-test-worker worker --serve-fd <fd> --cas-root <dir>
//                        --session-dir <dir> --worker-session-id <id>
//                        --runtime-id b3:<hex>
//       The session-program contract of a worker runtime
//       (svp/exec/worker/session_process.hpp): one session on <fd>, toy
//       outputs in the CAS at <dir>, results stamped with the session id and
//       runtime id. The worker driver ships this executable as a stand-in
//       runtime's bin/svp-builder.
//
// By default toy outputs live in memory; with `--cas-root <dir>` inputs are
// resolved from, and outputs stored in, the content-addressed cache at <dir>
// through CasTaskArtifactAccess, as a real worker does.
//
// Frames go out through TransitFaultFrameWriter, which corrupts a RESULT
// payload flagged kToyCorruptInTransit so the coordinator sees a payload hash
// mismatch exactly as it would from a faulty link.

#include "in_memory_artifact_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/worker_loop.hpp"
#include "toy_tasks.hpp"
#include "transit_fault_writer.hpp"

#include <csignal>
#include <cstdio>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include "pairing_test_support.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#endif

namespace {

using namespace svp::exec;

struct WorkerRuntime {
  std::string session_id = "ws_test_worker_" + std::to_string(::getpid());
  test::InMemoryArtifactStore memory_store;
  std::unique_ptr<CasTaskArtifactAccess> cas_access;
  TaskArtifactAccess* artifacts = &memory_store;
  TaskTypeRegistry registry;
};

// Returns false (after reporting why) when the cache cannot be opened.
bool set_up(WorkerRuntime& runtime, const std::map<std::string, std::string>& flags) {
  const test::ToyTaskOptions toy_options{.faults = test::ToyFaults::honoured};
  const auto cas_root = flags.find("--cas-root");
  if (cas_root == flags.end()) {
    test::register_toy_tasks(runtime.registry, runtime.memory_store, toy_options);
    return true;
  }
  CacheResult<CasStore> store = CasStore::at(cas_root->second);
  if (!store) {
    std::fprintf(stderr, "svp-exec-test-worker: cache %s: %s\n", cas_root->second.c_str(),
                 store.error().message.c_str());
    return false;
  }
  runtime.cas_access =
      std::make_unique<CasTaskArtifactAccess>(std::move(store).value(), runtime.session_id);
  runtime.artifacts = runtime.cas_access.get();
  test::register_toy_tasks(
      runtime.registry,
      [access = runtime.cas_access.get()](std::vector<std::byte> bytes, std::string media_type,
                                          std::string role) {
        return access->put(bytes, std::move(media_type), std::move(role));
      },
      toy_options);
  return true;
}

WorkerLoopExit serve(ByteStream& stream, WorkerRuntime& runtime, const std::string& session_id,
                     Blake3Digest runtime_id = {}) {
  StreamFrameReader reader(stream);
  test::TransitFaultFrameWriter writer(stream);
  return run_worker_loop(reader, writer, runtime.registry, *runtime.artifacts,
                         WorkerLoopOptions{.worker_session_id = session_id,
                                           .runtime_id = runtime_id});
}

int serve_session_fd(const std::map<std::string, std::string>& flags, WorkerRuntime& runtime) {
  for (const char* required :
       {"--serve-fd", "--cas-root", "--worker-session-id", "--runtime-id"}) {
    if (!flags.contains(required)) {
      std::cerr << "svp-exec-test-worker: worker mode needs " << required << "\n";
      return 2;
    }
  }
  const std::optional<Blake3Digest> runtime_id =
      parse_blake3_prefixed(flags.at("--runtime-id"));
  if (!runtime_id) {
    std::cerr << "svp-exec-test-worker: --runtime-id must be b3:<hex>\n";
    return 2;
  }
  const int fd = std::stoi(flags.at("--serve-fd"));
  FdByteStream stream(fd, fd);
  const WorkerLoopExit exit =
      serve(stream, runtime, flags.at("--worker-session-id"), *runtime_id);
  return exit == WorkerLoopExit::protocol_error ? 1 : 0;
}

int serve_stdio(WorkerRuntime& runtime) {
  FdByteStream stream(STDIN_FILENO, STDOUT_FILENO);
  return serve(stream, runtime, runtime.session_id) == WorkerLoopExit::protocol_error ? 1 : 0;
}

#if defined(__APPLE__)
int serve_listener(const std::map<std::string, std::string>& flags, WorkerRuntime& runtime) {
  if (!flags.contains("--psk-file") || !flags.contains("--pairing-id")) {
    std::cerr << "svp-exec-test-worker: --listen needs --psk-file and --pairing-id\n";
    return 2;
  }
  // Signals are taken synchronously by sigwait below, on this thread only.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

  remote::RemoteListenerOptions options;
  options.pairing =
      remote::test::read_pairing_file(flags.at("--pairing-id"), flags.at("--psk-file"));
  if (flags.contains("--service-name")) {
    options.service_name = flags.at("--service-name");
  }
  if (flags.contains("--port")) {
    options.port = static_cast<std::uint16_t>(std::stoul(flags.at("--port")));
  }
  remote::RemoteListener listener(
      options, [&](remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
        const std::string session_id =
            runtime.session_id + "_" + std::to_string(info.session_number);
        const WorkerLoopExit exit = serve(stream, runtime, session_id);
        std::cerr << "svp-exec-test-worker: session " << info.session_number << " from "
                  << info.peer << " ended (" << worker_loop_exit_name(exit) << ")\n";
      });
  listener.start();
  std::cout << "listening service=" << listener.advertised_name()
            << " port=" << listener.port() << std::endl;
  int signal_number = 0;
  sigwait(&stop_signals, &signal_number);
  listener.stop();
  return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  // The coordinator may vanish mid-write; report it as a write error.
  std::signal(SIGPIPE, SIG_IGN);

  bool listen = false;
  bool session_mode = false;
  std::map<std::string, std::string> flags;
  int first = 1;
  if (argc > 1 && std::string(argv[1]) == "worker") {
    session_mode = true;
    first = 2;
  }
  for (int index = first; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--listen") {
      listen = true;
    } else if (argument.starts_with("--") && index + 1 < argc) {
      flags[argument] = argv[++index];
    } else {
      std::cerr << "svp-exec-test-worker: unknown argument " << argument << "\n";
      return 2;
    }
  }

  WorkerRuntime runtime;
  if (!set_up(runtime, flags)) {
    return 2;
  }
  try {
    if (session_mode) {
      return serve_session_fd(flags, runtime);
    }
    if (!listen) {
      return serve_stdio(runtime);
    }
#if defined(__APPLE__)
    return serve_listener(flags, runtime);
#else
    std::cerr << "svp-exec-test-worker: --listen needs the Apple remote transport\n";
    return 2;
#endif
  } catch (const std::exception& error) {
    std::cerr << "svp-exec-test-worker: " << error.what() << "\n";
    return 1;
  }
}
