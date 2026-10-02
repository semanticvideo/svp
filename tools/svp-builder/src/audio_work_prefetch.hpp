#pragma once

// One dispatched run over several independent WAVs' audio work (the
// microphone streams, which the microphone stage processes independently,
// one after another): every stream's ASR chunks, or every stream's
// diarization windows, spread over every Mac at once. The stage then runs
// each stream exactly as before; its per-boundary dispatcher answers from
// the run's results when the boundary asks for exactly the work that was
// run, and otherwise dispatches (or does) that boundary's work on its own,
// so a prediction that misses never changes the result.

#include "engine/audio_work_dispatch.hpp"

#include "svp/audio/asr_chunk_run.hpp"
#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/diarization_window_map.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace svp::builder {

class PrefetchedAsrChunks {
 public:
  // Runs `works` now (nothing when `dispatch` is null or declines).
  PrefetchedAsrChunks(const engine::AudioWorkDispatch* dispatch,
                      std::vector<svp::audio::AsrChunkWork> works,
                      const engine::AudioProgress& on_progress);

  // The per-boundary dispatcher (svp::audio::AsrChunkDispatch); valid while
  // this object lives. Empty when `dispatch` was null.
  [[nodiscard]] svp::audio::AsrChunkDispatch boundary_dispatch() const;

 private:
  const engine::AudioWorkDispatch* dispatch_;
  std::vector<svp::audio::AsrChunkWork> works_;
  std::optional<std::vector<std::vector<std::optional<svp::audio::AsrChunkOutcome>>>> results_;
};

class PrefetchedDiarizationWindows {
 public:
  PrefetchedDiarizationWindows(const engine::AudioWorkDispatch* dispatch,
                               std::vector<svp::audio::DiarizationWindowWork> works,
                               const engine::AudioProgress& on_progress);

  [[nodiscard]] svp::audio::DiarizationWindowDispatch run_dispatch() const;

 private:
  const engine::AudioWorkDispatch* dispatch_;
  std::vector<svp::audio::DiarizationWindowWork> works_;
  std::optional<std::vector<std::vector<std::optional<svp::audio::DiarizationWindowMap>>>>
      results_;
};

// The chunk work execute_asr_boundary would offer its dispatcher for
// `boundary` (its model resolution, as execute_asr_boundary does it), or
// nullopt when the boundary would not run chunks.
[[nodiscard]] std::optional<svp::audio::AsrChunkWork> predicted_asr_chunk_work(
    const svp::audio::AsrExecutionBoundary& boundary, const std::filesystem::path& staging_root,
    const std::filesystem::path& model_cache_root,
    const svp::audio::WhisperRuntimeThreads& threads);

}  // namespace svp::builder
