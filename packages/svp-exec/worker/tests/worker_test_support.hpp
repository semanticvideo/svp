#pragma once

// Fixtures for the worker package tests: scratch directories, a socket-pair
// frame channel, a synthetic model cache, and command helpers.

#include "exec_test_support.hpp"
#include "storage_test_support.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/thread_plan.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace svp::exec::worker::test {

using svp::exec::test::expect;
using svp::exec::test::expect_equal;
using svp::exec::test::read_file;
using svp::exec::test::run_tests;
using svp::exec::test::TemporaryDirectory;
using svp::exec::test::write_file;

template <typename Function>
void expect_worker_error(WorkerErrorCode expected, Function&& function, std::string_view message) {
  try {
    function();
  } catch (const WorkerError& error) {
    if (error.code() != expected) {
      throw std::runtime_error(std::string(message) + ": expected " +
                               std::string(worker_error_code_name(expected)) + " but got " +
                               std::string(worker_error_code_name(error.code())) + " (" +
                               error.what() + ")");
    }
    return;
  }
  throw std::runtime_error(std::string(message) + ": no WorkerError was thrown");
}

// Runs a shell command; returns its exit status and stdout.
inline std::pair<int, std::string> run_command(const std::string& command) {
  std::string output;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    throw std::runtime_error("popen failed for " + command);
  }
  std::array<char, 4096> buffer{};
  while (std::size_t count = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
    output.append(buffer.data(), count);
  }
  const int status = ::pclose(pipe);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

// Both ends of a connected stream socket pair, as frame channels.
struct FrameChannel {
  int fds[2] = {-1, -1};
  std::unique_ptr<FdFrameReader> left_reader;
  std::unique_ptr<FdFrameWriter> left_writer;
  std::unique_ptr<FdFrameReader> right_reader;
  std::unique_ptr<FdFrameWriter> right_writer;

  FrameChannel() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      throw std::runtime_error("socketpair failed");
    }
    int one = 1;
    ::setsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    ::setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    left_reader = std::make_unique<FdFrameReader>(fds[0]);
    left_writer = std::make_unique<FdFrameWriter>(fds[0]);
    right_reader = std::make_unique<FdFrameReader>(fds[1]);
    right_writer = std::make_unique<FdFrameWriter>(fds[1]);
  }
  ~FrameChannel() {
    close_left();
    close_right();
  }
  void close_left() {
    if (fds[0] >= 0) {
      ::shutdown(fds[0], SHUT_RDWR);
      ::close(fds[0]);
      fds[0] = -1;
    }
  }
  void close_right() {
    if (fds[1] >= 0) {
      ::shutdown(fds[1], SHUT_RDWR);
      ::close(fds[1]);
      fds[1] = -1;
    }
  }
};

// A thread plan with every count fixed, as a coordinator sends it.
inline svp::models::ThreadPlan fixed_thread_plan() {
  svp::models::ThreadPlan plan = svp::models::resolve_local_thread_plan(
      svp::models::detect_host_cpu_topology(), /*ocr_recognition_workers=*/1);
  for (svp::models::OrtThreadCounts* counts :
       {&plan.ocr_detection, &plan.ocr_recognition, &plan.depth, &plan.visual_entity_detection,
        &plan.visual_entity_embedding, &plan.text_embedding, &plan.speech_activity,
        &plan.forced_alignment}) {
    if (counts->intra_op == svp::models::kRuntimeChoosesThreadCount) counts->intra_op = 1;
    if (counts->inter_op == svp::models::kRuntimeChoosesThreadCount) counts->inter_op = 1;
  }
  return plan;
}

inline std::string file_digest(const std::filesystem::path& path) {
  return "blake3:" + svp::models::blake3_hex_for_file(path);
}

// A model bundle in the svp-models layout under <cache>/<model_id>, and its
// manifest JSON.
inline nlohmann::json write_model_bundle(const std::filesystem::path& cache_root,
                                         const std::string& model_id,
                                         const std::string& weights) {
  const std::filesystem::path root = cache_root / model_id;
  std::filesystem::create_directories(root);
  write_file(root / "weights.bin", weights);
  write_file(root / "LICENSE", "synthetic test license\n");
  write_file(root / "NOTICE", "synthetic test notice\n");
  nlohmann::json manifest = {
      {"schema_version", "svp-model-bundle-1"},
      {"model_bundle_id", model_id + "@1.0+blake3_000000000000"},
      {"model_id", model_id},
      {"model_version", "1.0"},
      {"bundle_blake3",
       "blake3:0000000000000000000000000000000000000000000000000000000000000000"},
      {"runtime", "onnxruntime"},
      {"format", "onnx"},
      {"license", "Apache-2.0"},
      {"supported_execution_providers", nlohmann::json::array({"cpu"})},
      {"files", nlohmann::json::array({{{"path", "weights.bin"},
                                        {"role", "weights"},
                                        {"blake3", file_digest(root / "weights.bin")}},
                                       {{"path", "LICENSE"},
                                        {"role", "license"},
                                        {"blake3", file_digest(root / "LICENSE")}},
                                       {{"path", "NOTICE"},
                                        {"role", "notice"},
                                        {"blake3", file_digest(root / "NOTICE")}}})},
      {"input_contract", nlohmann::json::object()},
      {"output_contract", nlohmann::json::object()},
      {"preprocessor_contract", nlohmann::json::object()},
      {"postprocessor_contract", nlohmann::json::object()},
  };
  write_file(root / "model.svpmodel.json", manifest.dump(2) + "\n");
  const std::string digest = "blake3:" + svp::models::blake3_hex_for_model_bundle(root);
  manifest["bundle_blake3"] = digest;
  manifest["model_bundle_id"] = model_id + "@1.0+blake3_" + digest.substr(7, 12);
  write_file(root / "model.svpmodel.json", manifest.dump(2) + "\n");
  return manifest;
}

inline void write_model_lock(const std::filesystem::path& cache_root,
                             const std::vector<nlohmann::json>& manifests) {
  nlohmann::json models = nlohmann::json::array();
  for (const nlohmann::json& manifest : manifests) {
    models.push_back({{"model_id", manifest.at("model_id")},
                      {"model_bundle_id", manifest.at("model_bundle_id")},
                      {"model_version", manifest.at("model_version")},
                      {"bundle_blake3", manifest.at("bundle_blake3")},
                      {"files", manifest.at("files")}});
  }
  write_file(cache_root / "model-lock.json",
             nlohmann::json{{"schema_version", "svp-model-lock-1"},
                            {"model_set_id", "worker-test-set"},
                            {"models", std::move(models)}}
                     .dump(2) +
                 "\n");
}

}  // namespace svp::exec::worker::test
