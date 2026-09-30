#pragma once

#include "svp/models/thread_plan.hpp"

namespace svp::audio {

// The ThreadPlan entries whisper.cpp inference uses: decode and VAD threads,
// plus the ONNX Runtime session of the CTC forced aligner it runs per chunk.
struct WhisperRuntimeThreads {
  svp::models::WhisperThreadCounts whisper;
  svp::models::OrtThreadCounts forced_alignment;
};

[[nodiscard]] inline WhisperRuntimeThreads whisper_runtime_threads(
    const svp::models::ThreadPlan& plan) {
  return {.whisper = plan.whisper, .forced_alignment = plan.forced_alignment};
}

}  // namespace svp::audio
