#pragma once

#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_extraction_plan.hpp"
#include "svp/models/thread_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <vector>

namespace svp::builder {

struct MicrophoneAsrStageResult {
  svp::audio::AsrExecutionBoundary boundary;
  nlohmann::json stream_results = nlohmann::json::array();
  nlohmann::json reconciliation = nlohmann::json::object();
  nlohmann::json processor_record = nlohmann::json::object();
};

using MicrophoneAsrProgressCallback =
    std::function<void(std::size_t current, std::size_t total)>;

struct MicrophoneDiarizationProgressCallbacks {
  std::function<void()> started;
  MicrophoneAsrProgressCallback progress;
};

[[nodiscard]] MicrophoneAsrStageResult run_microphone_asr_stage(
    const svp::audio::AudioExtractionPlan& extraction_plan,
    // Per microphone analysis stream, in plan order: whether extraction
    // staged it (AudioExtractionRun::microphone_analysis_streams[i].success).
    const std::vector<bool>& microphone_streams_staged,
    std::int64_t media_duration_us,
    bool model_runtime_available,
    bool asr_model_available,
    bool asr_model_verified,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& model_cache_root,
    const svp::models::ThreadPlan& thread_plan,
    MicrophoneAsrProgressCallback progress = {},
    MicrophoneDiarizationProgressCallbacks diarization_progress = {});

}  // namespace svp::builder
