#pragma once

// The audio lane as two stages so a resumed build can skip extraction:
//   extract:    per-stream FLAC, analysis WAV, waveform, loudness, spectrum,
//               and the VAD boundary (media/audio/, provenance/processors.jsonl);
//   transcribe: ASR, diarization (or the microphone path), and the transcript
//               writer (transcript/).
// Running extract then transcribe is exactly the former single audio stage.

#include "build_pipeline_internal.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

namespace svp::builder {

// What transcription needs from extraction.
struct AudioExtractStageState {
  // The audio foundation record so far (plan, extraction, VAD boundary).
  nlohmann::json audio_json = nlohmann::json::object();
  bool analysis_audio_written = false;
  // Per microphone analysis stream, in plan order: staged successfully.
  std::vector<bool> microphone_streams_staged;
};

[[nodiscard]] nlohmann::json audio_extract_stage_state_to_json(
    const AudioExtractStageState& state);
[[nodiscard]] AudioExtractStageState audio_extract_stage_state_from_json(
    const nlohmann::json& value);

// Whether transcription takes the microphone path (more than one microphone
// analysis stream). The audio plan fixes it, so a planner knows it before any
// stage runs.
[[nodiscard]] bool audio_uses_microphone_path(const BuildPipelineOptions& options,
                                              const svp::media::MediaIngestPlan& plan,
                                              bool model_runtime_available);

[[nodiscard]] AudioExtractStageState run_audio_extract_stage(
    BuildPipelineContext& context);

// Sets context.output["audio_foundation"]. Returns the lane's exit status when
// it must stop the build (diarization required but unavailable).
[[nodiscard]] std::optional<int> run_audio_transcribe_stage(
    BuildPipelineContext& context, const AudioExtractStageState& extracted);

}  // namespace svp::builder
