#pragma once

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
  std::vector<std::string> required_outputs;
  std::vector<std::string> pending_processors;
  std::vector<std::string> blockers;
};

[[nodiscard]] AudioStagePlan build_audio_stage_plan(
    const std::filesystem::path& source_path,
    const svp::media::MediaProbe& probe,
    bool ffmpeg_audio_extraction_available);

[[nodiscard]] nlohmann::json audio_stage_plan_to_json(const AudioStagePlan& plan);

}  // namespace svp::audio
