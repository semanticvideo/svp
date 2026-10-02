#pragma once

// diarize.window (plan §2.3 diarization row, §2.4 item 8, M5): one window of
// sherpa-onnx diarization's map (svp/audio/diarization_window_map.hpp):
// segmentation of its feed pieces and the bounded embedding of each
// window-local observation. Clustering, track collapse, fingerprints, and
// word assignment stay on the coordinator (run_sherpa_diarization's fold).
//
// sherpa-onnx is loaded with dlopen and is not part of the runtime identity
// unless it comes from the runtime bundle, so each task names the BLAKE3 of
// the library the coordinator loaded; a runtime that loaded another library
// refuses the task (retryably), so a window's map never depends on which
// sherpa-onnx build a Mac happens to have.

#include "svp/audio/diarization_window_map.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::audio::tasks {

inline constexpr std::string_view kDiarizeWindowTaskType = "diarize.window";
// Changes whenever the parameter schema, the payload, or their meaning does.
inline constexpr std::uint64_t kDiarizeWindowTaskTypeVersion = 1;
inline constexpr std::string_view kDiarizeWindowLane = "diarize.window";
inline constexpr std::string_view kDiarizeWindowMapRole = "diarization_window_map";
inline constexpr std::string_view kDiarizeWindowMapMediaType = "application/cbor";

// Admission estimate of one task's peak resident memory (plan §4.4): the
// pyannote segmentation and eres2net embedding sessions (sherpa-onnx's own
// ONNX Runtime) and the window's samples as floats (a 304 s window at 16 kHz
// is about 19 MB). A whole diarization of 636 s of speech in one process
// (svp-builder diarize: every window's map plus the fold) peaked at
// 294 MiB; 512 MB leaves margin. It sizes admission and slots only.
inline constexpr std::uint64_t kDiarizeWindowEstimatedPeakRssMb = 512;

// How long one second of window audio is expected to take, for leases and
// the attempt deadline only, when this Mac has no measurement (a
// --distributed build replaces it with its measured cost): capacity
// calibration measured about 0.027 s per second of audio on one slot of an
// M4 mini; 0.05 s keeps leases generous for a slower Mac.
inline constexpr double kDiarizeWindowDefaultSecondsPerAudioSecond = 0.05;

struct DiarizeWindowParameters {
  std::uint64_t window_index = 0;
  // The WAV's sample count (the windows are cut from it; a WAV of another
  // length is refused).
  std::uint64_t sample_count = 0;
  bool compute_embeddings = true;
  svp::models::SherpaThreadCounts threads;
  std::string model_id;
  // "b3:<hex>" of the sherpa-onnx C API library the coordinator loaded.
  std::string sherpa_library;
};

[[nodiscard]] nlohmann::json diarize_window_parameters_to_json(
    const DiarizeWindowParameters& parameters);
[[nodiscard]] DiarizeWindowParameters diarize_window_parameters_from_json(
    const nlohmann::json& value);
[[nodiscard]] std::optional<std::string> validate_diarize_window_parameters(
    const nlohmann::json& value);

struct DiarizeWindowTaskInputs {
  std::string build_session_id;
  // Distinguishes the WAVs of one run (microphone streams).
  std::uint64_t run_ordinal = 0;
  svp::exec::ArtifactRef audio;
  svp::exec::TaskModelRef model_ref;
  svp::audio::DiarizationWindowWork work;
  std::string sherpa_library;
  // Measured (or default) seconds per second of window audio.
  double seconds_per_audio_second = kDiarizeWindowDefaultSecondsPerAudioSecond;
};

// The validated TaskSpec for window `window_index`. Throws
// std::invalid_argument for a window outside the work.
[[nodiscard]] svp::exec::TaskSpec make_diarize_window_task_spec(
    const DiarizeWindowTaskInputs& inputs, std::size_t window_index);

[[nodiscard]] svp::exec::TaskOrderKey diarize_window_order_key(std::uint64_t run_ordinal,
                                                               std::size_t window_index);

// The window's map, or nullopt when the task records it as not mapped there.
// Throws std::invalid_argument for outputs that do not match the spec.
[[nodiscard]] std::optional<svp::audio::DiarizationWindowMap> read_diarize_window_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads);

// "b3:<hex>" of the sherpa-onnx library this process loaded (loading it
// first, as is_sherpa_diarization_available does); nullopt when none loads.
// Thread-safe.
[[nodiscard]] std::optional<std::string> loaded_sherpa_library_identity();

// "b3:<hex>" of the library this process would load, without loading it
// (svp::audio::sherpa_lib_path_expected); nullopt when there is none.
[[nodiscard]] std::optional<std::string> expected_sherpa_library_identity();

}  // namespace svp::audio::tasks
