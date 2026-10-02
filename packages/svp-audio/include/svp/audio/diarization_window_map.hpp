#pragma once

// The per-window map of sherpa-onnx diarization (plan §2.3, M5), split from
// run_sherpa_diarization so a build may run it as a task on another Mac.
//
// run_sherpa_diarization cuts the analysis WAV into fixed windows
// (build_diarization_windows: 300 s accepted ranges, each processed with 2 s
// of context on both sides) and, for each window, creates a fresh sherpa-onnx
// diarization pipeline, feeds it the window in 5 s pieces, clips each piece's
// segments to the window's accepted range, groups them into window-local
// speakers, and computes one bounded speaker embedding per window-local
// observation. Nothing in that work reads another window: a window's map is
// a pure function of its samples, the two models, and the thread counts.
//
// Everything global stays in run_sherpa_diarization (the fold): observation
// IDs, cannot-link constraints, clustering, track collapse, fingerprints, and
// word assignment. It folds the windows' maps in window order, so a map
// computed here or anywhere else gives the same result.

#include "svp/audio/audio_dispatch_error.hpp"
#include "svp/audio/sherpa_diarization_segment.hpp"
#include "svp/models/thread_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

// A window-local speaker within one 5 s feed piece of a window.
struct DiarizationPieceSpeaker {
  // sherpa-onnx's speaker label within the piece.
  int32_t local_speaker = 0;
  // Its segments in the piece, clipped to the window's accepted range,
  // sorted by start (speaker_id is local_speaker).
  std::vector<SherpaDiarizationSegment> segments;
  // The window-local observation it belongs to, numbered from 0 in the
  // order the window first sees them.
  std::size_t window_observation = 0;
  // True when this speaker starts that observation (it then carries the
  // observation's embedding).
  bool starts_observation = false;
  // The observation's bounded embedding (compute_bounded_speaker_embedding);
  // empty when embeddings are not computed or this speaker continues an
  // earlier observation.
  std::vector<float> embedding;
};

// One feed piece: its speakers in ascending local label order.
struct DiarizationWindowPiece {
  std::vector<DiarizationPieceSpeaker> speakers;
};

// One window's map. `pieces` is empty for a window with no samples.
struct DiarizationWindowMap {
  std::size_t window_index = 0;
  std::vector<DiarizationWindowPiece> pieces;
};

// Where a window's map could not be made: the blocker run_sherpa_diarization
// records for it (a pipeline that does not create, a piece sherpa-onnx
// returns nothing for, a WAV read error).
struct DiarizationWindowFailure {
  std::string blocker;
};

// The map of one window, or why it could not be made.
struct DiarizationWindowOutcome {
  std::optional<DiarizationWindowMap> map;
  std::optional<DiarizationWindowFailure> failure;
};

// The windows run_sherpa_diarization cuts a WAV of `sample_count` samples
// into.
[[nodiscard]] std::size_t diarization_window_count(std::size_t sample_count);

// The 5 s feed pieces window `window_index` of a `sample_count`-sample WAV
// is processed in (its progress units). Throws std::out_of_range for a
// window outside the WAV.
[[nodiscard]] std::size_t diarization_window_piece_count(std::size_t sample_count,
                                                         std::size_t window_index);

// The samples window `window_index` of a `sample_count`-sample WAV is
// processed over (its accepted range plus context). Throws std::out_of_range
// for a window outside the WAV.
[[nodiscard]] std::size_t diarization_window_samples(std::size_t sample_count,
                                                     std::size_t window_index);

// The settings a window map runs with; run_sherpa_diarization's own.
struct DiarizationWindowSettings {
  svp::models::SherpaThreadCounts threads;
  // Whether observations carry embeddings: run_sherpa_diarization computes
  // them only when its speaker embedding extractor was created.
  bool compute_embeddings = true;
};

// The PCM16 mono 16 kHz WAV's sample count. Throws std::runtime_error when
// the file is not one.
[[nodiscard]] std::size_t diarization_wav_sample_count(const std::filesystem::path& wav_path);

// Runs window `window_index` of `wav_path` on its own: loads sherpa-onnx (as
// is_sherpa_diarization_available does), creates the pipeline and, when
// settings.compute_embeddings, an embedding extractor from `model_dir`, and
// maps the window exactly as run_sherpa_diarization does. Returns a failure
// (never throws for sherpa-onnx or WAV problems) when the library, a model,
// the extractor, or the window's work cannot be had here. `on_piece` is
// called after each feed piece.
[[nodiscard]] DiarizationWindowOutcome run_diarization_window_map(
    const std::filesystem::path& wav_path, const std::filesystem::path& model_dir,
    std::size_t window_index, const DiarizationWindowSettings& settings,
    const std::function<void()>& on_piece = {});

// A diarization run's per-window work, offered to a dispatcher.
struct DiarizationWindowWork {
  std::filesystem::path wav_path;
  std::size_t sample_count = 0;
  std::size_t window_count = 0;
  DiarizationWindowSettings settings;
};

// Runs a diarization run's windows elsewhere. Returns nullopt to have every
// window mapped here (nothing could be dispatched), otherwise one entry per
// window in window order: a map, or nullopt for a window to map here (its
// task could not deliver one, for example it failed where it ran). Progress
// is reported in feed pieces (diarization_window_piece_count). A dispatcher
// that cannot deliver at all throws AudioDispatchError, which the fold never
// turns into a blocker.
using DiarizationWindowDispatch =
    std::function<std::optional<std::vector<std::optional<DiarizationWindowMap>>>(
        const DiarizationWindowWork& work, const DiarizationProgressCallback& on_progress)>;

}  // namespace svp::audio
