#pragma once

// asr.chunk_batch (plan §2.3 ASR row, M5): a contiguous run of one ASR
// boundary's chunks on the fixed chunk grid, each run by
// svp::audio::run_asr_chunk on its slice (with pre-context) of the boundary's
// analysis WAV. The coordinator folds the chunks' words in chunk order with
// reconcile_overlapping_chunks, unchanged (execute_asr_boundary).

#include "svp/audio/asr_chunk_run.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svp::audio::tasks {

inline constexpr std::string_view kAsrChunkBatchTaskType = "asr.chunk_batch";
// Changes whenever the parameter schema, the payload, or their meaning does.
inline constexpr std::uint64_t kAsrChunkBatchTaskTypeVersion = 1;
inline constexpr std::string_view kAsrChunkBatchLane = "asr.chunk_batch";
inline constexpr std::string_view kAsrChunkOutcomesRole = "asr_chunk_outcomes";
inline constexpr std::string_view kAsrChunkOutcomesMediaType = "application/cbor";

// Admission estimate of one task's peak resident memory (plan §4.4): a
// process running ASR chunks holds the Whisper small.en context (its weights
// in Metal buffers, which count as resident on Apple Silicon), one decoding
// state, the Silero VAD context, and the wav2vec2 phoneme aligner's ONNX
// Runtime session. A process running 25 chunks twice in a row
// (svp-audio-task-tests) peaked at 1,918 MiB; 2,304 MB leaves about 20% for a
// chunk whose temperature fallback regrows the KV cache. It sizes admission
// and slots only; it never affects output.
inline constexpr std::uint64_t kAsrChunkBatchEstimatedPeakRssMb = 2304;

// One chunk of a batch: its position in its boundary's chunk plan and the
// plan entry itself.
struct AsrChunkBatchItem {
  std::uint64_t ordinal = 0;
  svp::audio::AsrChunkPlan chunk;
};

// Everything a task needs besides the WAV bytes and the runtime's own models:
// the chunks, the model IDs (the spec's model_refs name exactly these), and
// the thread counts, so no worker fills a value from its own host.
struct AsrChunkBatchParameters {
  std::vector<AsrChunkBatchItem> chunks;
  std::string model_id;
  std::string vad_model_id;
  std::optional<std::string> alignment_model_id;
  svp::audio::WhisperRuntimeThreads threads;
};

[[nodiscard]] nlohmann::json asr_chunk_batch_parameters_to_json(
    const AsrChunkBatchParameters& parameters);
// Strict inverse; throws std::invalid_argument with the validator's reason.
[[nodiscard]] AsrChunkBatchParameters asr_chunk_batch_parameters_from_json(
    const nlohmann::json& value);
// nullopt when valid: known fields only, at least one chunk, ascending
// ordinals, every thread count at least 1.
[[nodiscard]] std::optional<std::string> validate_asr_chunk_batch_parameters(
    const nlohmann::json& value);

struct AsrChunkBatchTaskInputs {
  std::string build_session_id;
  // Distinguishes the boundaries of one run (microphone streams): part of
  // every task ID and the first ordinal of the order key.
  std::uint64_t boundary_ordinal = 0;
  // The boundary's analysis WAV (role kAudioTaskInputRole).
  svp::exec::ArtifactRef audio;
  // Whisper, VAD, and (when aligning) aligner bundles, in any order.
  std::vector<svp::exec::TaskModelRef> model_refs;
  svp::audio::AsrChunkWork work;
  svp::vision::tasks::ItemBatchPolicy batch_policy;
};

// Bytes one chunk adds to a batch's parameters and, at most, to its output
// (a chunk's slice is shorter than whisper's 30 s window, so its words are
// bounded by the decoder's text context), for partition_items.
[[nodiscard]] svp::vision::tasks::ItemBytes asr_chunk_item_bytes(
    const svp::audio::AsrChunkPlan& chunk, std::uint64_t ordinal);

// The validated TaskSpec for chunks [batch.first, batch.first + batch.count)
// of inputs.work.chunks. Throws std::invalid_argument for a batch outside
// them.
[[nodiscard]] svp::exec::TaskSpec make_asr_chunk_batch_task_spec(
    const AsrChunkBatchTaskInputs& inputs, const svp::vision::tasks::ItemBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey asr_chunk_batch_order_key(
    std::uint64_t boundary_ordinal, const svp::vision::tasks::ItemBatch& batch);

// One entry per spec chunk, in order: the outcome whisper ran, or nullopt for
// a chunk the task records as not run there. Throws std::invalid_argument
// for outputs that do not match the spec.
[[nodiscard]] std::vector<std::optional<svp::audio::AsrChunkOutcome>> read_asr_chunk_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads);

}  // namespace svp::audio::tasks
