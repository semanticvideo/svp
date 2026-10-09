#pragma once

// The audio lane as two stages so a resumed build can skip extraction:
//   extract:    per-stream FLAC, analysis WAV, waveform, loudness, spectrum,
//               and the VAD boundary (media/audio/, and its processor records
//               fragment, processor_provenance.hpp);
//   transcribe: ASR, diarization (or the microphone path, which also stages a
//               processor records fragment), and the transcript writer
//               (transcript/).
// Running extract then transcribe is exactly the former single audio stage.

#include "build_pipeline_internal.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace svp::builder {

namespace engine {
struct AudioWorkDispatch;
}  // namespace engine

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

// Why the transcribe stage stopped the build: the exit status it asks for and
// a one-line reason for the build's error message.
struct AudioStageExit {
  int exit_code = kBuildFailedExitCode;
  std::string reason;
};

// Sets context.output["audio_foundation"]. Returns the lane's exit when it
// must stop the build (diarization required but unavailable); the
// diarization stage then reports failed.
// `dispatch` (--distributed only, else null) runs ASR chunks and
// diarization windows as tasks (engine/audio_work_dispatch.hpp).
[[nodiscard]] std::optional<AudioStageExit> run_audio_transcribe_stage(
    BuildPipelineContext& context, const AudioExtractStageState& extracted,
    const engine::AudioWorkDispatch* dispatch = nullptr);

}  // namespace svp::builder
