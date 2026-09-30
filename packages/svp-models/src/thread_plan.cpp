#include "svp/models/thread_plan.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace svp::models {
namespace {

// Largest whisper.cpp decode thread count SVP asks for: the backend's former
// kMaximumInferenceThreads, moved here unchanged so the plan reproduces the
// previous min(hardware_concurrency, 8) decode width. whisper.cpp output can
// depend on n_threads, so changing this cap is an output change.
constexpr unsigned kMaximumWhisperDecodeThreads = 8;

// whisper.cpp 1.8.6 whisper_vad_default_context_params().n_threads. SVP's VAD
// time mapper used that default, and whisper_full's internal VAD context
// still does (it is not configurable), so both VAD passes stay aligned.
constexpr int kWhisperVadThreads = 4;

// sherpa-onnx segmentation and speaker-embedding sessions have always run
// single-threaded in SVP. sherpa-onnx is loaded from an unpinned dylib, so
// one thread keeps its results independent of the host's core count.
constexpr int kSherpaThreads = 1;

void check_ort(const OrtThreadCounts& counts, std::string_view role,
               std::vector<std::string>& problems) {
  if (counts.intra_op < 0) {
    problems.push_back(std::string(role) + ".intra_op must be >= 0");
  }
  if (counts.inter_op < 0) {
    problems.push_back(std::string(role) + ".inter_op must be >= 0");
  }
}

void check_positive(int value, std::string_view field,
                    std::vector<std::string>& problems) {
  if (value <= 0) {
    problems.push_back(std::string(field) + " must be > 0");
  }
}

bool ort_is_fixed(const OrtThreadCounts& counts) {
  return counts.intra_op != kRuntimeChoosesThreadCount &&
         counts.inter_op != kRuntimeChoosesThreadCount;
}

}  // namespace

HostCpuTopology detect_host_cpu_topology() {
  return {.logical_cpus = std::thread::hardware_concurrency()};
}

int onnx_runtime_default_pool_threads(const HostCpuTopology& host) {
#if defined(__APPLE__)
  // ONNX Runtime 1.23.2 (vcpkg) sizes a pool whose thread count is 0 from
  // PosixEnv::GetNumPhysicalCpuCores(). Its cpuinfo path is compiled out on
  // __APPLE__ (onnxruntime/core/platform/posix/env.cc), leaving
  // DefaultNumCores() = max(1, hardware_concurrency() / 2). This holds for
  // both the intra-op pool and the ORT_PARALLEL inter-op pool. Measured on an
  // M4 (10 logical CPUs): an explicit 5 is byte-identical to 0, 4/6/10 are not.
  return std::max(1, static_cast<int>(host.logical_cpus / 2));
#else
  // Elsewhere ONNX Runtime asks cpuinfo for physical cores, which SVP does
  // not link; leave the choice to ONNX Runtime.
  (void)host;
  return kRuntimeChoosesThreadCount;
#endif
}

ThreadPlan resolve_local_thread_plan(const HostCpuTopology& host,
                                     int ocr_recognition_workers) {
  const int ort_threads = onnx_runtime_default_pool_threads(host);
  const OrtThreadCounts ort{.intra_op = ort_threads, .inter_op = ort_threads};

  ThreadPlan plan;
  plan.ocr_detection = ort;
  plan.ocr_recognition = ort;
  plan.depth = ort;
  plan.visual_entity_detection = ort;
  plan.visual_entity_embedding = ort;
  plan.text_embedding = ort;
  plan.speech_activity = ort;
  plan.forced_alignment = ort;
  plan.ocr_recognition_workers = ocr_recognition_workers;
  plan.whisper.decode = static_cast<int>(std::clamp(
      host.logical_cpus, 1U, kMaximumWhisperDecodeThreads));
  plan.whisper.vad = kWhisperVadThreads;
  plan.sherpa.segmentation = kSherpaThreads;
  plan.sherpa.embedding = kSherpaThreads;
  plan.opencv = kRuntimeChoosesThreadCount;
  return plan;
}

std::vector<std::string> thread_plan_problems(const ThreadPlan& plan) {
  std::vector<std::string> problems;
  check_ort(plan.ocr_detection, "onnx_runtime.ocr_detection", problems);
  check_ort(plan.ocr_recognition, "onnx_runtime.ocr_recognition", problems);
  check_ort(plan.depth, "onnx_runtime.depth", problems);
  check_ort(plan.visual_entity_detection,
            "onnx_runtime.visual_entity_detection", problems);
  check_ort(plan.visual_entity_embedding,
            "onnx_runtime.visual_entity_embedding", problems);
  check_ort(plan.text_embedding, "onnx_runtime.text_embedding", problems);
  check_ort(plan.speech_activity, "onnx_runtime.speech_activity", problems);
  check_ort(plan.forced_alignment, "onnx_runtime.forced_alignment", problems);
  check_positive(plan.ocr_recognition_workers, "ocr_recognition_workers",
                 problems);
  check_positive(plan.whisper.decode, "whisper.decode_threads", problems);
  check_positive(plan.whisper.vad, "whisper.vad_threads", problems);
  check_positive(plan.sherpa.segmentation, "sherpa.segmentation_threads",
                 problems);
  check_positive(plan.sherpa.embedding, "sherpa.embedding_threads", problems);
  if (plan.opencv < 0) {
    problems.push_back("opencv_threads must be >= 0");
  }
  return problems;
}

void require_valid_thread_plan(const ThreadPlan& plan) {
  const std::vector<std::string> problems = thread_plan_problems(plan);
  if (problems.empty()) return;
  std::string message = "invalid thread plan:";
  for (const std::string& problem : problems) {
    message += " " + problem + ";";
  }
  throw std::invalid_argument(message);
}

bool thread_plan_is_host_independent(const ThreadPlan& plan) {
  return thread_plan_problems(plan).empty() &&
         ort_is_fixed(plan.ocr_detection) &&
         ort_is_fixed(plan.ocr_recognition) && ort_is_fixed(plan.depth) &&
         ort_is_fixed(plan.visual_entity_detection) &&
         ort_is_fixed(plan.visual_entity_embedding) &&
         ort_is_fixed(plan.text_embedding) &&
         ort_is_fixed(plan.speech_activity) &&
         ort_is_fixed(plan.forced_alignment);
}

}  // namespace svp::models
