#include "builder_worker_tasks.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/builder/runtime_tools.hpp"
#include "coordinator_context.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/worker/runtime_store.hpp"
#include "svp/exec/worker/worker_agent.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_layout.hpp"
#include "svp/exec/worker_loop.hpp"
#include "workers_cli.hpp"

#include <csignal>
#include <iostream>

namespace svp::builder::workers {

using namespace svp::exec::worker;

int run_worker_serve(const WorkerCliOptions& options) {
  WorkerAgentOptions agent;
  agent.root = options.root;
  if (options.memory_reserve_floor_mb != 0) {
    agent.admission.reserve_floor_bytes = options.memory_reserve_floor_mb * 1024ULL * 1024ULL;
  }
  // The agent runs from a runtime directory (<root>/runtimes/<id>/bin/...);
  // name that runtime in HELLO_ACK and the log. Its files were verified when
  // the runtime was installed; every session's runtime is verified again
  // before it starts.
  const std::filesystem::path runtime_dir = current_executable().parent_path().parent_path();
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
  const RuntimeToolChoice ffmpeg = resolve_runtime_tool(
      RuntimeTool::ffmpeg, std::nullopt, locate_runtime_bundle(current_executable()),
      process_environment());
  // sherpa-onnx the way a local build of this runtime finds it: the
  // bundle's pinned library before the unpinned locations (diarize.window
  // tasks still refuse any library but the coordinator's).
  if (const std::optional<RuntimeToolChoice> sherpa = bundled_runtime_tool(
          RuntimeTool::sherpa_onnx, locate_runtime_bundle(current_executable()))) {
    svp::audio::set_sherpa_bundled_lib_path(sherpa->path);
  }
  svp::exec::TaskTypeRegistry registry;
  register_builder_worker_task_types(
      registry, artifacts,
      WorkerTaskEnvironment{.session_dir = options.session_dir.empty()
                                               ? std::filesystem::current_path()
                                               : std::filesystem::path(options.session_dir),
                            .model_store = options.model_store,
                            .ffmpeg_path = ffmpeg.path});
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
