#pragma once

// Shared helpers for svp-exec tests: assertions, byte conversion, and the
// sample records whose canonical bytes are pinned as golden vectors.

#include "svp/exec/exec_error.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace svp::exec::test {

inline void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

inline void expect_equal(std::string_view actual, std::string_view expected,
                         std::string_view message) {
  if (actual != expected) {
    throw std::runtime_error(std::string(message) + "\n  expected: " +
                             std::string(expected) + "\n  actual:   " +
                             std::string(actual));
  }
}

template <typename Function>
void expect_exec_error(ExecErrorCode expected, Function&& function,
                       std::string_view message) {
  try {
    function();
  } catch (const ExecError& error) {
    if (error.code() != expected) {
      throw std::runtime_error(
          std::string(message) + ": expected " +
          std::string(exec_error_code_name(expected)) + " but got " +
          std::string(exec_error_code_name(error.code())) + " (" + error.what() +
          ")");
    }
    return;
  }
  throw std::runtime_error(std::string(message) + ": no ExecError was thrown");
}

inline std::vector<std::byte> to_bytes(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char character : text) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  return bytes;
}

inline std::string to_text(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

inline Blake3Digest repeated_digest(std::uint8_t byte) {
  Blake3Digest digest{};
  digest.fill(byte);
  return digest;
}

inline TaskSpec sample_task_spec() {
  TaskSpec spec;
  spec.build_session_id = "bs_0001";
  spec.task_id = "task.toy.upper.chunk_000";
  spec.task_type = "toy.upper";
  spec.task_type_version = 1;
  spec.depends_on = {"task.plan.ingest"};
  spec.model_refs = {TaskModelRef{
      .model_id = "model_toy_upper",
      .model_bundle_id = "model_toy_upper@1.0.0+blake3_111111111111",
      .bundle_blake3 = repeated_digest(0x11)}};
  spec.inputs.emplace("source", ArtifactRef{.blake3 = repeated_digest(0x22),
                                            .bytes = 11,
                                            .media_type = "text/plain",
                                            .role = "source_text"});
  spec.parameters = nlohmann::json{{"sample_indices", {120, 121}},
                                   {"ratio", 0.5},
                                   {"case", "upper"}};
  spec.parameters_blake3 = compute_parameters_blake3(spec.parameters);
  spec.cache_key = repeated_digest(0x33);
  spec.resources = TaskResources{
      .est_peak_rss_mb = 64, .est_cpu_threads = 1, .est_seconds = 2};
  return spec;
}

inline TaskExecution sample_execution() {
  return TaskExecution{
      .worker_session_id = "ws_0001",
      .runtime_id = repeated_digest(0x44),
      .timing_ms = TaskTimingMs{
          .queue = 3, .input_fetch = 0, .decode = 1310, .compute = 9850, .encode = 4},
      .cpu_ms = TaskCpuMs{.user = 51234, .system = 812},
      .peak_rss_bytes = 1221541888};
}

inline TaskResult sample_task_result() {
  TaskResult result;
  result.task_id = "task.toy.upper.chunk_000";
  result.attempt = 1;
  result.status = TaskStatus::succeeded;
  result.outputs = {ArtifactRef{.blake3 = repeated_digest(0x55),
                                .bytes = 48213,
                                .media_type = "application/x-ndjson",
                                .role = "ocr_frame_detections"}};
  result.output_digest = compute_output_digest(result.outputs);
  result.execution = sample_execution();
  result.diagnostics = nlohmann::json{{"box_count", 146}};
  return result;
}

using TestCase = std::pair<const char*, void (*)()>;

inline int run_tests(std::string_view suite,
                     std::initializer_list<TestCase> tests) {
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << suite << ": " << name << " FAILED: " << error.what() << "\n";
    }
  }
  if (failures != 0) {
    return 1;
  }
  std::cout << suite << ": PASS (" << tests.size() << " tests)\n";
  return 0;
}

}  // namespace svp::exec::test
