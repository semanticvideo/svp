#pragma once

#include "svp/audio/audio_extraction_plan.hpp"
#include "svp/audio/vad_task_plan.hpp"
#include "svp/audio/vad_execution_boundary.hpp"
#include "svp/media/media_probe.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace svp::audio {

struct AudioStagePlan {
  std::filesystem::path source_path;
  bool source_audio_present = false;
  std::string selected_audio_stream_id;
  AudioExtractionPlan extraction_plan;
  VadTaskPlan vad_task_plan;
  VadExecutionBoundary vad_execution_boundary;
  std::vector<std::string> required_outputs;
  std::vector<std::string> pending_processors;
  std::vector<std::string> blockers;
};

[[nodiscard]] AudioStagePlan build_audio_stage_plan(
    const std::filesystem::path& source_path,
    const svp::media::MediaProbe& probe,
    bool ffmpeg_audio_extraction_available,
    const std::filesystem::path& ffmpeg_path = "ffmpeg",
    bool model_runtime_available = false);

[[nodiscard]] nlohmann::json audio_stage_plan_to_json(const AudioStagePlan& plan);

}  // namespace svp::audio
