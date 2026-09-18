#pragma once

#include "svp/audio/loudness_meter.hpp"
#include "svp/media/media_probe.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

struct AudioExtractionCommandPlan {
  std::string task_id;
  std::string source_audio_stream_id;
  std::int32_t source_stream_index = 0;
  std::string output_ref;
  std::vector<std::string> arguments;
};

struct AnalysisAudioCommandPlan {
  std::string task_id;
  std::vector<std::string> depends_on;
  std::string selected_source_audio_stream_id;
  std::int32_t source_stream_index = -1;
  std::int64_t source_start_us = 0;
  std::optional<std::int64_t> timeline_duration_us;
  std::string output_ref;
  std::vector<std::string> arguments;
};

struct AudioAbsenceArtifactPlan {
  std::string task_id;
  std::vector<std::string> depends_on;
  std::string processor_id;
  std::string output_ref;
};

struct WaveformArtifactPlan {
  std::string task_id;
  std::vector<std::string> depends_on;
  std::string processor_id;
  std::string input_ref;
  std::string output_ref;
  std::int64_t window_duration_us = 10000;
};

struct LoudnessStreamTarget {
  std::string source_audio_stream_id;
  std::int32_t source_stream_index = 0;
  std::int32_t channels = 0;
  std::int32_t sample_rate = 0;
  std::int64_t stream_start_us = 0;
  std::string input_ref;
};

struct LoudnessArtifactPlan {
  std::string task_id;
  std::vector<std::string> depends_on;
  std::string processor_id;
  std::vector<LoudnessStreamTarget> targets;
  std::string output_ref;
  std::string summary_output_ref;
  std::int64_t window_duration_us = kLoudnessWindowDurationUs;
};

struct AudioProvenanceArtifactPlan {
  std::string task_id;
  std::vector<std::string> depends_on;
  std::string output_ref;
};

struct AudioExtractionPlan {
  std::filesystem::path source_path;
  std::filesystem::path ffmpeg_path = "ffmpeg";
  std::filesystem::path ffprobe_path = "ffprobe";
  bool ffmpeg_available = false;
  bool source_audio_present = false;
  std::vector<AudioExtractionCommandPlan> original_streams;
  std::vector<AnalysisAudioCommandPlan> microphone_analysis_streams;
  AnalysisAudioCommandPlan analysis_audio;
  AudioAbsenceArtifactPlan audio_absence;
  WaveformArtifactPlan waveform;
  LoudnessArtifactPlan loudness;
  AudioProvenanceArtifactPlan processor_provenance;
  std::vector<std::string> blockers;
};

[[nodiscard]] AudioExtractionPlan build_audio_extraction_plan(
    const std::filesystem::path& source_path,
    const svp::media::MediaProbe& probe,
    bool ffmpeg_available,
    const std::filesystem::path& ffmpeg_path = "ffmpeg",
    const std::filesystem::path& ffprobe_path = "ffprobe");

[[nodiscard]] nlohmann::json audio_extraction_plan_to_json(
    const AudioExtractionPlan& plan);

}  // namespace svp::audio
