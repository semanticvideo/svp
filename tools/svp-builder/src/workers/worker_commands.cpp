#include "builder_worker_tasks.hpp"
#include "svp/builder/runtime_tools.hpp"
#include "coordinator_context.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/worker/runtime_store.hpp"
#include "svp/exec/worker/service_link.hpp"
#include "svp/exec/worker/worker_agent.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_layout.hpp"
#include "svp/exec/worker_loop.hpp"
#include "workers_cli.hpp"

#include <csignal>
#include <iostream>
#include <mach-o/dyld.h>
#include <vector>

namespace svp::builder::workers {

using namespace svp::exec::worker;

namespace {

// The program path this process was started with, symlinks NOT resolved
// (launchd passes ProgramArguments[0]); empty when unknown.
std::filesystem::path launched_program_path() {
  std::uint32_t size = 0;
  (void)::_NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1, '\0');
  if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::path(buffer.data());
}

}  // namespace

int run_worker_serve(const WorkerCliOptions& options) {
  WorkerAgentOptions agent;
  agent.root = options.root;
  if (options.memory_reserve_floor_mb != 0) {
    agent.admission.reserve_floor_bytes = options.memory_reserve_floor_mb * 1024ULL * 1024ULL;
  }
  // The agent runs from a runtime directory (<root>/runtimes/<id>/bin/...,
  // reached through <root>/current); name that runtime in HELLO_ACK and the
  // log. Its files were verified when the runtime was installed; every
  // session's runtime is verified again before it starts.
  const std::filesystem::path runtime_dir = current_executable().parent_path().parent_path();
  agent.launched_through_current =
      launched_through_current(WorkerLayout{.root = agent.root}, launched_program_path());
  try {
    agent.agent_runtime_id = svp::exec::compute_runtime_id(
        svp::exec::load_runtime_manifest(runtime_manifest_path(runtime_dir)));
  } catch (const std::exception&) {
    agent.agent_runtime_id.reset();
  }
  return run_worker_agent(agent);
}

int run_worker_session(const WorkerCliOptions& options) {
  std::signal(SIGPIPE, SIG_IGN);
  const std::optional<svp::exec::Blake3Digest> runtime_id =
      svp::exec::parse_blake3_prefixed(options.runtime_id);
  if (!runtime_id || options.cas_root.empty() || options.worker_session_id.empty() ||
      options.model_store.empty()) {
    std::cerr << "svp-builder worker: --serve-fd needs --cas-root, --model-store, "
                 "--worker-session-id, and --runtime-id b3:<hex>\n";
    return 2;
  }
  svp::exec::CacheResult<svp::exec::CasStore> store = svp::exec::CasStore::at(options.cas_root);
  if (!store) {
    std::cerr << "svp-builder worker: cache " << options.cas_root << ": "
              << store.error().message << "\n";
    return 1;
  }
  svp::exec::CasTaskArtifactAccess artifacts(std::move(store).value(), options.worker_session_id);
  // ffmpeg the way a local build of this runtime resolves it: the bundle
  // next to this svp-builder, $SVP_FFMPEG, or ffmpeg on the job's PATH.
  const std::optional<RuntimeBundle> bundle = locate_runtime_bundle(current_executable());
  const RuntimeToolChoice ffmpeg =
      resolve_runtime_tool(RuntimeTool::ffmpeg, std::nullopt, bundle, process_environment());
  // ffprobe too, for whole-video jobs, which run a complete build.
  RuntimeToolSelection tools;
  if (bundle) {
    tools.bundle_root = bundle->root;
    tools.runtime_id = bundle->runtime_id;
  }
  tools.ffmpeg = ffmpeg;
  tools.ffprobe =
      resolve_runtime_tool(RuntimeTool::ffprobe, std::nullopt, bundle, process_environment());
  tools.uses_sherpa = true;
  // sherpa-onnx the way a local build of this runtime finds it: the
  // bundle's pinned library before the unpinned locations (diarize.window
  // tasks still refuse any library but the coordinator's).
  tools.sherpa_bundled = offer_bundled_sherpa_library(bundle);
  svp::exec::TaskTypeRegistry registry;
  register_builder_worker_task_types(
      registry, artifacts,
      WorkerTaskEnvironment{.session_dir = options.session_dir.empty()
                                               ? std::filesystem::current_path()
                                               : std::filesystem::path(options.session_dir),
                            .model_store = options.model_store,
                            .ffmpeg_path = ffmpeg.path,
                            .cas_root = options.cas_root,
                            .worker_session_id = options.worker_session_id,
                            .tools = tools});
  const svp::exec::WorkerLoopExit exit = svp::exec::run_worker_loop(
      options.serve_fd, options.serve_fd, registry, artifacts,
      svp::exec::WorkerLoopOptions{.worker_session_id = options.worker_session_id,
                                   .runtime_id = *runtime_id});
  return exit == svp::exec::WorkerLoopExit::protocol_error ? 1 : 0;
}

int run_worker_verify_runtime(const WorkerCliOptions& options) {
  std::optional<svp::exec::Blake3Digest> expected;
  if (!options.expect.empty()) {
    expected = svp::exec::parse_blake3_prefixed(options.expect);
    if (!expected) {
      std::cerr << "svp-builder worker verify-runtime: --expect must be b3:<hex>\n";
      return 2;
    }
  }
  const svp::exec::Blake3Digest runtime_id =
      verify_runtime_directory(options.runtime_dir, expected);
  std::cout << "runtime_id=" << svp::exec::blake3_prefixed(runtime_id) << "\n";
  return 0;
}

}  // namespace svp::builder::workers
