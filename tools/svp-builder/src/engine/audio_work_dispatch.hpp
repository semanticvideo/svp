#pragma once

// The dispatchers a --distributed build gives its audio transcription stage
// (M5): ASR chunks (asr.chunk_batch) and diarization windows
// (diarize.window). Each, when the stage reaches that work:
//   1. names the WAV the tasks read by BLAKE3 (the staged analysis audio, or
//      a microphone's), so this Mac's slots resolve it and every worker
//      session is supplied it;
//   2. cuts the work into tasks (chunks by the measured seconds per chunk,
//      item_batch_policy.hpp; one task per diarization window);
//   3. runs them on this Mac's slots and the workers (run_subtasks), with the
//      scheduler's leases, retries, and quarantine;
//   4. returns the results in chunk / window order, wherever they ran.
// The stage folds them exactly as it folds its own (execute_asr_boundary,
// run_sherpa_diarization): any chunk or window without a result is computed
// by the stage itself. A dispatcher returns nullopt, so the stage does all
// of its work itself as a local build does, when no worker takes the task
// type, its models are not the ones the workers were given, or this Mac
// could not measure it.
//
// Several WAVs (microphone streams, which the stage processes
// independently) can be dispatched in one run, so their work spreads over
// every Mac at once.

#include "engine/vision_dispatch_setup.hpp"

#include "svp/audio/asr_chunk_run.hpp"
#include "svp/audio/diarization_window_map.hpp"
#include "svp/exec/task_registry.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace svp::builder::engine {

class StageOutputAccess;

using AudioProgress = std::function<void(std::size_t done, std::size_t total)>;

struct AudioWorkDispatch {
  // One entry per work, in order: that work's per-chunk results
  // (svp::audio::AsrChunkDispatch). nullopt: run every chunk here.
  std::function<std::optional<std::vector<std::vector<std::optional<svp::audio::AsrChunkOutcome>>>>(
      const std::vector<svp::audio::AsrChunkWork>& works, const AudioProgress& on_progress)>
      asr_chunks;
  // One entry per work, in order: that work's per-window maps
  // (svp::audio::DiarizationWindowDispatch). nullopt: map every window here.
  std::function<std::optional<std::vector<std::vector<std::optional<svp::audio::DiarizationWindowMap>>>>(
      const std::vector<svp::audio::DiarizationWindowWork>& works,
      const AudioProgress& on_progress)>
      diarization_windows;
};

// The per-boundary hooks svp-audio takes, over one work each. Empty when
// `dispatch` is null.
[[nodiscard]] svp::audio::AsrChunkDispatch single_asr_chunk_dispatch(
    const AudioWorkDispatch* dispatch);
[[nodiscard]] svp::audio::DiarizationWindowDispatch single_diarization_window_dispatch(
    const AudioWorkDispatch* dispatch);

// What the audio dispatchers need beyond the vision setup: measuring a type
// on this Mac in its stage (DistributedFleet::measure_in_stage).
struct AudioDispatchExtras {
  std::function<std::optional<DispatchedTypeCapacity>(std::string_view task_type)>
      measure_in_stage;
};

// `registry` holds the audio task types for this Mac's slots and
// `artifacts` resolves the WAVs for them; both must outlive the returned
// hooks.
[[nodiscard]] AudioWorkDispatch make_audio_work_dispatch(
    std::shared_ptr<const VisionDispatchSetup> setup, AudioDispatchExtras extras,
    const svp::exec::TaskTypeRegistry& registry, StageOutputAccess& artifacts);

}  // namespace svp::builder::engine
