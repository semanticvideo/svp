#pragma once

// One ASR chunk, split from execute_asr_boundary's chunk loop so a build may
// run it as a task on another Mac (plan §2.3, §2.4 item 6, M5).
//
// A chunk is a pure function of its samples: it is sliced from the analysis
// WAV with its pre-context (plan_asr_chunk_context), decoded by whisper.cpp
// with a fresh decoding state (no_context, the cached context holds only the
// model weights), optionally re-timed by the phoneme aligner, and trimmed to
// its nominal range (retain_nominal_chunk_words). The boundary then folds the
// chunks' words in chunk order with reconcile_overlapping_chunks, unchanged,
// wherever each chunk ran.

#include "svp/audio/asr_chunk_context.hpp"
#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/audio_dispatch_error.hpp"
#include "svp/audio/whisper_runtime_threads.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

// The files one chunk runs with.
struct AsrChunkModels {
  // The Whisper ggml bundle directory.
  std::filesystem::path model_dir;
  // The Silero VAD ggml file.
  std::filesystem::path vad_model_path;
  // The phoneme aligner bundle directory; empty runs without alignment.
  std::filesystem::path aligner_model_dir;
};

// One chunk's result, as the boundary's loop uses it.
struct AsrChunkOutcome {
  // Whisper ran on the chunk. When false, `blockers` says why and the chunk
  // contributes no words.
  bool ran = false;
  // The words inside the chunk's nominal range, with its ordinal.
  std::vector<AsrWord> words;
  // whisper's alignment status for the chunk (not_requested, applied,
  // applied_partial, fallback).
  std::string alignment_status;
  std::vector<std::string> blockers;
  // WhisperInferenceResult::alignment_error: set when alignment fell back by
  // throwing, which depends on the host rather than the chunk.
  std::string alignment_error;
  // Words whisper decoded over the whole slice, before trimming (memory
  // diagnostics only).
  std::size_t decoded_word_count = 0;
};

// Slices chunk `chunk` (ordinal `chunk_ordinal`) of `input_wav` into
// `slice_dir`, runs it, and removes the slice. Throws what whisper.cpp
// inference or slicing throws.
[[nodiscard]] AsrChunkOutcome run_asr_chunk(const std::filesystem::path& input_wav,
                                            const AsrChunkPlan& chunk,
                                            std::int64_t chunk_ordinal,
                                            const AsrChunkModels& models,
                                            const WhisperRuntimeThreads& threads,
                                            const std::filesystem::path& slice_dir);

// The model files of `model_id`, `vad_model_id`, and (when set)
// `alignment_model_id` under `model_cache_root`, each verified against its
// manifest as execute_asr_boundary verifies them. nullopt, with `why`, when a
// bundle is missing or does not verify.
[[nodiscard]] std::optional<AsrChunkModels> resolve_asr_chunk_models(
    const std::filesystem::path& model_cache_root, const std::string& model_id,
    const std::string& vad_model_id, const std::optional<std::string>& alignment_model_id,
    std::string& why);

// A boundary's chunk work, offered to a dispatcher.
struct AsrChunkWork {
  std::filesystem::path input_wav;
  std::vector<AsrChunkPlan> chunks;
  std::string model_id;
  std::string vad_model_id;
  // Set when the boundary aligns words (its aligner bundle verified).
  std::optional<std::string> alignment_model_id;
  WhisperRuntimeThreads threads;
};

// Runs a boundary's chunks elsewhere. Returns nullopt to run every chunk here
// (nothing could be dispatched), otherwise one entry per chunk in chunk
// order: an outcome whisper ran, or nullopt for a chunk to run here (its task
// could not deliver it, for example it failed where it ran). Progress is in
// chunks. A dispatcher that cannot deliver at all throws AudioDispatchError.
using AsrChunkDispatch =
    std::function<std::optional<std::vector<std::optional<AsrChunkOutcome>>>(
        const AsrChunkWork& work,
        const std::function<void(std::size_t current, std::size_t total)>& on_progress)>;

}  // namespace svp::audio
