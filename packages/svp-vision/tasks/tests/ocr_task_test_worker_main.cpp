// svp-vision-ocr-task-test-worker: serves run_worker_loop on stdin/stdout
// with ocr.frame_batch registered, as the loopback executor's child.
//
//   svp-vision-ocr-task-test-worker --cas-root <dir> --model-cache <dir>
//                                   --ffmpeg <path>
//
// Inputs are resolved from, and outputs stored in, the content-addressed
// cache at --cas-root through CasTaskArtifactAccess, as a real worker does.

#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/worker_loop.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <csignal>
#include <cstdio>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
  // The coordinator may vanish mid-write; report it as a write error.
  std::signal(SIGPIPE, SIG_IGN);

  std::map<std::string, std::string> flags;
  for (int index = 1; index + 1 < argc; index += 2) {
    flags[argv[index]] = argv[index + 1];
  }
  for (const char* required : {"--cas-root", "--model-cache", "--ffmpeg"}) {
    if (!flags.contains(required)) {
      std::cerr << "svp-vision-ocr-task-test-worker: missing " << required << "\n";
      return 2;
    }
  }

  try {
    svp::exec::CacheResult<svp::exec::CasStore> store =
        svp::exec::CasStore::at(flags.at("--cas-root"));
    if (!store) {
      std::cerr << "svp-vision-ocr-task-test-worker: cache: " << store.error().message
                << "\n";
      return 2;
    }
    const std::string session_id = "ws_ocr_test_worker_" + std::to_string(::getpid());
    svp::exec::CasTaskArtifactAccess artifacts(std::move(store).value(), session_id);

    svp::exec::TaskTypeRegistry registry;
    svp::vision::tasks::register_ocr_frame_batch_task(
        registry,
        svp::vision::tasks::OcrFrameBatchWorkerEnvironment{
            .model_cache_root = flags.at("--model-cache"),
            .ffmpeg_path = flags.at("--ffmpeg"),
            .write_output =
                [&artifacts](std::span<const std::byte> bytes, std::string media_type,
                             std::string role) {
                  return artifacts.put(bytes, std::move(media_type), std::move(role));
                },
        });

    const svp::exec::WorkerLoopExit exit = svp::exec::run_worker_loop(
        STDIN_FILENO, STDOUT_FILENO, registry, artifacts,
        svp::exec::WorkerLoopOptions{.worker_session_id = session_id});
    return exit == svp::exec::WorkerLoopExit::protocol_error ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "svp-vision-ocr-task-test-worker: " << error.what() << "\n";
    return 1;
  }
}
