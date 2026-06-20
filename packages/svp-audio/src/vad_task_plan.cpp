#include "svp/audio/vad_task_plan.hpp"

#include "svp/media/canonical_timing.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

constexpr std::int64_t kDefaultVadChunkDurationUs = 30000000;

std::string vad_task_id(std::size_t ordinal) {
  std::ostringstream output;
  output << "task.vad.audio_chunk_" << std::setw(6) << std::setfill('0') << ordinal;
  return output.str();
}

std::string speech_output_ref(std::size_t ordinal) {
  std::ostringstream output;
  output << "transcript/speech_regions.chunk_" << std::setw(6) << std::setfill('0')
         << ordinal << ".jsonl";
  return output.str();
}

std::optional<std::int64_t> duration_from_probe(const svp::media::MediaProbe& probe) {
  if (probe.container_timing.has_value() && probe.container_timing->duration_pts.has_value()) {
    return svp::media::pts_to_microseconds(*probe.container_timing->duration_pts,
                                           probe.container_timing->timebase);
  }
  if (!probe.audio_streams.empty() &&
      probe.audio_streams.front().timing.duration_pts.has_value()) {
    return svp::media::pts_to_microseconds(
        *probe.audio_streams.front().timing.duration_pts,
        probe.audio_streams.front().timing.timebase);
  }
  return std::nullopt;
}

nlohmann::json task_to_json(const VadTaskBoundary& task) {
  return {
      {"task_id", task.task_id},
      {"task_type", "vad"},
      {"depends_on", task.depends_on},
      {"processor_id", task.processor_id},
      {"model_refs", task.model_refs},
      {"input_refs", task.input_refs},
      {"output_refs", task.output_refs},
      {"start_us", task.start_us},
      {"end_us", task.end_us},
      {"start_sec", svp::media::microseconds_to_seconds_string(task.start_us)},
      {"end_sec", svp::media::microseconds_to_seconds_string(task.end_us)},
  };
}

}  // namespace

VadTaskPlan build_vad_task_plan(const svp::media::MediaProbe& probe,
                                std::optional<std::int64_t> duration_us,
                                bool model_runtime_available) {
  VadTaskPlan plan;
  plan.chunk_duration_us = kDefaultVadChunkDurationUs;
  plan.chunk_overlap_us = 0;

  if (!model_runtime_available) {
    plan.blockers.push_back("ONNX Runtime VAD execution is not wired in this foundation pass");
  }
  if (probe.audio_streams.empty()) {
    plan.blockers.push_back("source has no audio; VAD task plan waits for canonical silence generation");
  }

  const std::optional<std::int64_t> resolved_duration_us =
      duration_us.has_value() ? duration_us : duration_from_probe(probe);
  if (!resolved_duration_us.has_value()) {
    plan.blockers.push_back("media duration is unavailable; VAD chunk boundaries cannot be planned");
    return plan;
  }
  if (*resolved_duration_us < 0) {
    throw std::invalid_argument("VAD duration must be non-negative");
  }
  if (*resolved_duration_us == 0) {
    return plan;
  }

  for (std::int64_t start_us = 0; start_us < *resolved_duration_us;
       start_us += plan.chunk_duration_us) {
    const std::int64_t end_us =
        std::min(start_us + plan.chunk_duration_us, *resolved_duration_us);
    const std::size_t ordinal = plan.tasks.size();
    plan.tasks.push_back(VadTaskBoundary{
        vad_task_id(ordinal),
        start_us,
        end_us,
        {"task.audio.analysis.astream_000"},
        {"media/audio/analysis_mono_16k.wav"},
        {speech_output_ref(ordinal)},
        {plan.model_id},
        plan.processor_id,
    });
  }

  return plan;
}

nlohmann::json vad_task_plan_to_json(const VadTaskPlan& plan) {
  nlohmann::json tasks = nlohmann::json::array();
  for (const VadTaskBoundary& task : plan.tasks) {
    tasks.push_back(task_to_json(task));
  }

  return {
      {"processor_id", plan.processor_id},
      {"model_id", plan.model_id},
      {"runtime", plan.runtime},
      {"execution_provider", plan.execution_provider},
      {"chunk_duration_us", plan.chunk_duration_us},
      {"chunk_overlap_us", plan.chunk_overlap_us},
      {"tasks", tasks},
      {"blockers", plan.blockers},
      {"vad_run", false},
      {"speech_regions_written", false},
  };
}

}  // namespace svp::audio
