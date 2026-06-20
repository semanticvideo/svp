#include "svp/audio/vad_execution_boundary.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace svp::audio {
namespace {

void push_unique(std::vector<std::string>& values, const std::string& value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

}  // namespace

VadExecutionBoundary build_vad_execution_boundary(const VadTaskPlan& plan,
                                                  bool analysis_audio_available,
                                                  bool waveform_available,
                                                  bool model_runtime_available) {
  VadExecutionBoundary boundary;
  boundary.processor_id = plan.processor_id;
  boundary.model_id = plan.model_id;
  boundary.runtime = plan.runtime;
  boundary.execution_provider = plan.execution_provider;
  boundary.analysis_audio_available = analysis_audio_available;
  boundary.waveform_available = waveform_available;
  boundary.model_runtime_available = model_runtime_available;
  boundary.input_refs = {
      "media/audio/analysis_mono_16k.wav",
      "media/audio/waveform.jsonl",
  };

  for (const VadTaskBoundary& task : plan.tasks) {
    boundary.task_ids.push_back(task.task_id);
    for (const std::string& output_ref : task.output_refs) {
      push_unique(boundary.staged_chunk_output_refs, output_ref);
    }
  }

  boundary.blockers = plan.blockers;
  if (!analysis_audio_available) {
    boundary.blockers.push_back("analysis audio is not staged for VAD execution");
  }
  if (!waveform_available) {
    boundary.blockers.push_back("waveform envelope is not staged for VAD execution");
  }
  if (!model_runtime_available) {
    boundary.blockers.push_back("ONNX Runtime VAD execution is not wired in this boundary pass");
  }
  if (plan.tasks.empty()) {
    boundary.blockers.push_back("no VAD chunk tasks are available to execute");
  }

  return boundary;
}

nlohmann::json vad_execution_boundary_to_json(const VadExecutionBoundary& boundary) {
  return {
      {"processor_id", boundary.processor_id},
      {"model_id", boundary.model_id},
      {"runtime", boundary.runtime},
      {"execution_provider", boundary.execution_provider},
      {"task_ids", boundary.task_ids},
      {"input_refs", boundary.input_refs},
      {"staged_chunk_output_refs", boundary.staged_chunk_output_refs},
      {"final_output_ref", boundary.final_output_ref},
      {"analysis_audio_available", boundary.analysis_audio_available},
      {"waveform_available", boundary.waveform_available},
      {"model_runtime_available", boundary.model_runtime_available},
      {"vad_run", boundary.vad_run},
      {"speech_regions_written", boundary.speech_regions_written},
      {"speech_region_count", boundary.speech_region_count},
      {"blockers", boundary.blockers},
  };
}

}  // namespace svp::audio
