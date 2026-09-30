#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::models {

// Thread counts for every native runtime a build uses. Some runtimes change
// serialized numbers when their thread count changes (an ONNX Runtime
// recognizer intra-op cap of 2 changed OCR confidences; see
// docs/ocr_performance_notes.md), so a build resolves one ThreadPlan when it
// starts and passes it to every consumer instead of letting each runtime ask
// the host. A distributed coordinator sends its plan to workers so output
// never depends on which machine ran a task.

// "Let the runtime choose from the host." Only meaningful for runtimes whose
// own default we cannot reproduce exactly on this platform (ONNX Runtime off
// Apple, OpenCV everywhere). A plan containing it is not host-independent.
inline constexpr int kRuntimeChoosesThreadCount = 0;

struct OrtThreadCounts {
  int intra_op = kRuntimeChoosesThreadCount;
  int inter_op = kRuntimeChoosesThreadCount;

  friend bool operator==(const OrtThreadCounts&, const OrtThreadCounts&) = default;
};

struct WhisperThreadCounts {
  // whisper_full_params::n_threads.
  int decode = kRuntimeChoosesThreadCount;
  // whisper_vad_context_params::n_threads for SVP's own VAD time mapper.
  int vad = kRuntimeChoosesThreadCount;

  friend bool operator==(const WhisperThreadCounts&,
                         const WhisperThreadCounts&) = default;
};

struct SherpaThreadCounts {
  int segmentation = kRuntimeChoosesThreadCount;
  int embedding = kRuntimeChoosesThreadCount;

  friend bool operator==(const SherpaThreadCounts&,
                         const SherpaThreadCounts&) = default;
};

struct ThreadPlan {
  // ONNX Runtime session threads, one entry per model role.
  OrtThreadCounts ocr_detection;
  OrtThreadCounts ocr_recognition;
  OrtThreadCounts depth;
  OrtThreadCounts visual_entity_detection;
  OrtThreadCounts visual_entity_embedding;
  OrtThreadCounts text_embedding;
  OrtThreadCounts speech_activity;
  OrtThreadCounts forced_alignment;

  // PP-OCR recognition worker threads (each calls the recognizer session).
  int ocr_recognition_workers = 1;
  WhisperThreadCounts whisper;
  SherpaThreadCounts sherpa;
  // Not applied: SVP never calls cv::setNumThreads, so OpenCV keeps its own
  // default. Recorded so the plan names every runtime.
  int opencv = kRuntimeChoosesThreadCount;

  friend bool operator==(const ThreadPlan&, const ThreadPlan&) = default;
};

struct HostCpuTopology {
  // std::thread::hardware_concurrency(); 0 when the host cannot report it.
  unsigned logical_cpus = 0;
};

[[nodiscard]] HostCpuTopology detect_host_cpu_topology();

// ONNX Runtime's own intra/inter-op pool size for a session whose thread
// count is left at 0, reproduced for this platform, or
// kRuntimeChoosesThreadCount where it cannot be reproduced exactly.
[[nodiscard]] int onnx_runtime_default_pool_threads(const HostCpuTopology& host);

// The plan a local build uses: every value equals what each runtime chose
// from the host before thread counts were explicit.
[[nodiscard]] ThreadPlan resolve_local_thread_plan(const HostCpuTopology& host,
                                                   int ocr_recognition_workers);

// Empty when the plan is usable. Counts must be >= 0; runtimes with no
// "choose for me" value (OCR workers, whisper, sherpa) must be > 0.
[[nodiscard]] std::vector<std::string> thread_plan_problems(const ThreadPlan& plan);

// Throws std::invalid_argument listing thread_plan_problems().
void require_valid_thread_plan(const ThreadPlan& plan);

// True when no value is kRuntimeChoosesThreadCount, i.e. the plan fixes every
// runtime that SVP configures. OpenCV is excluded because SVP does not
// configure it (see ThreadPlan::opencv).
[[nodiscard]] bool thread_plan_is_host_independent(const ThreadPlan& plan);

struct ThreadPlanOverride {
  std::string environment_variable;
  std::string field;
  int value = 0;
};

using EnvironmentLookup =
    std::function<std::optional<std::string>(std::string_view name)>;

[[nodiscard]] EnvironmentLookup process_environment_lookup();

// Applies the diagnostic SVP_OCR_* thread environment variables on top of the
// plan and returns what changed. The recognition-worker variable is honoured
// only when builder memory diagnostics are enabled, as before plans existed.
std::vector<ThreadPlanOverride> apply_thread_plan_environment_overrides(
    ThreadPlan& plan,
    const EnvironmentLookup& lookup,
    bool diagnostic_overrides_enabled);

enum class ThreadPlanSource { host, supplied };

struct ThreadPlanResolution {
  ThreadPlan plan;
  ThreadPlanSource source = ThreadPlanSource::host;
  HostCpuTopology host;
  std::vector<ThreadPlanOverride> overrides;
};

[[nodiscard]] nlohmann::json thread_plan_to_json(const ThreadPlan& plan);
// Strict inverse of thread_plan_to_json; throws std::invalid_argument.
[[nodiscard]] ThreadPlan thread_plan_from_json(const nlohmann::json& value);
[[nodiscard]] nlohmann::json thread_plan_resolution_to_json(
    const ThreadPlanResolution& resolution);

}  // namespace svp::models
