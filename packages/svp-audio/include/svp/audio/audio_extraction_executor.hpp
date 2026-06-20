#pragma once

#include "svp/audio/audio_extraction_plan.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

struct AudioExtractionCommandRun {
  std::string task_id;
  std::string output_ref;
  std::filesystem::path staged_output_path;
  std::vector<std::string> arguments;
  bool command_available = false;
  bool command_safe = false;
  bool command_executed = false;
  bool success = false;
  std::optional<int> exit_code;
  std::string skipped_reason;
};

struct AudioExtractionRun {
  std::filesystem::path staging_root;
  std::vector<AudioExtractionCommandRun> original_streams;
  AudioExtractionCommandRun analysis_audio;
  std::vector<std::string> blockers;
  bool extraction_run = false;
  bool original_streams_written = false;
  bool analysis_audio_written = false;
};

[[nodiscard]] AudioExtractionRun execute_audio_extraction_plan(
    const AudioExtractionPlan& plan,
    const std::filesystem::path& staging_root);

[[nodiscard]] nlohmann::json audio_extraction_run_to_json(
    const AudioExtractionRun& run);

}  // namespace svp::audio
