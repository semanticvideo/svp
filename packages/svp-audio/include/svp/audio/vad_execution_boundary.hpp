#pragma once

#include "svp/audio/vad_task_plan.hpp"

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace svp::audio {

struct VadExecutionBoundary {
  std::string processor_id;
  std::string model_id;
  std::string runtime;
  std::string execution_provider;
  std::vector<std::string> task_ids;
  std::vector<std::string> input_refs;
  std::vector<std::string> staged_chunk_output_refs;
  std::string final_output_ref = "transcript/speech_regions.jsonl";
  std::vector<std::string> blockers;
  bool analysis_audio_available = false;
  bool waveform_available = false;
  bool model_runtime_available = false;
  bool vad_run = false;
  bool speech_regions_written = false;
  std::size_t speech_region_count = 0;
};

[[nodiscard]] VadExecutionBoundary build_vad_execution_boundary(
    const VadTaskPlan& plan,
    bool analysis_audio_available,
    bool waveform_available,
    bool model_runtime_available);

[[nodiscard]] nlohmann::json vad_execution_boundary_to_json(
    const VadExecutionBoundary& boundary);

}  // namespace svp::audio
