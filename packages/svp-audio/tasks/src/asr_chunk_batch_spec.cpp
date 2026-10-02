#include "svp/audio/tasks/asr_chunk_batch.hpp"

#include "audio_task_spec.hpp"
#include "task_outputs.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace svp::audio::tasks {
namespace {

// A chunk's slice is at most one chunk plus its pre-context, far below
// whisper's 30 s decode window, so its words are bounded by the decoder's
// text context of 448 tokens (whisper.cpp n_text_ctx for every Whisper
// model); a word record (its text, two timestamps, a confidence, an ordinal,
// and a timing source in CBOR) stays well under 256 bytes. Only batch sizing
// uses the bound (no RESULT may outgrow the frame limit).
constexpr std::uint64_t kWhisperTextContextTokens = 448;
constexpr std::uint64_t kAsrWordRecordBytes = 256;

}  // namespace

svp::vision::tasks::ItemBytes asr_chunk_item_bytes(const svp::audio::AsrChunkPlan& chunk,
                                                   std::uint64_t ordinal) {
  AsrChunkBatchParameters one;
  one.chunks.push_back({.ordinal = ordinal, .chunk = chunk});
  one.model_id = "m";
  one.vad_model_id = "v";
  one.threads.whisper = {.decode = 1, .vad = 1};
  one.threads.forced_alignment = {.intra_op = 1, .inter_op = 1};
  const std::uint64_t parameter_bytes =
      asr_chunk_batch_parameters_to_json(one).at("chunks").at(0).dump().size();
  return {.parameter_bytes = parameter_bytes,
          .result_bytes = kWhisperTextContextTokens * kAsrWordRecordBytes};
}

svp::exec::TaskOrderKey asr_chunk_batch_order_key(std::uint64_t boundary_ordinal,
                                                  const svp::vision::tasks::ItemBatch& batch) {
  return svp::exec::TaskOrderKey{.lane = std::string(kAsrChunkBatchLane),
                                 .ordinals = {boundary_ordinal, batch.first}};
}

svp::exec::TaskSpec make_asr_chunk_batch_task_spec(const AsrChunkBatchTaskInputs& inputs,
                                                   const svp::vision::tasks::ItemBatch& batch) {
  const std::vector<svp::audio::AsrChunkPlan>& chunks = inputs.work.chunks;
  if (batch.count == 0 || batch.first >= chunks.size() ||
      batch.count > chunks.size() - batch.first) {
    throw std::invalid_argument("asr.chunk_batch batch is outside the chunks");
  }
  AsrChunkBatchParameters parameters;
  for (std::uint64_t index = batch.first; index < batch.first + batch.count; ++index) {
    parameters.chunks.push_back({.ordinal = index, .chunk = chunks[index]});
  }
  parameters.model_id = inputs.work.model_id;
  parameters.vad_model_id = inputs.work.vad_model_id;
  parameters.alignment_model_id = inputs.work.alignment_model_id;
  parameters.threads = inputs.work.threads;
  return detail::assemble_audio_task_spec(detail::AudioSpecAssembly{
      .build_session_id = inputs.build_session_id,
      .task_type = std::string(kAsrChunkBatchTaskType),
      .task_type_version = kAsrChunkBatchTaskTypeVersion,
      .task_id = detail::audio_task_id(kAsrChunkBatchTaskType, inputs.boundary_ordinal,
                                       batch.first, batch.first + batch.count - 1),
      .model_refs = inputs.model_refs,
      .audio = inputs.audio,
      .parameters = asr_chunk_batch_parameters_to_json(parameters),
      .resources = svp::exec::TaskResources{
          .est_peak_rss_mb = kAsrChunkBatchEstimatedPeakRssMb,
          .est_cpu_threads = static_cast<std::uint64_t>(
              std::max({1, inputs.work.threads.whisper.decode,
                        inputs.work.threads.forced_alignment.intra_op})),
          .est_seconds =
              svp::vision::tasks::item_batch_estimated_seconds(inputs.batch_policy, batch.count)},
  });
}

std::vector<std::optional<svp::audio::AsrChunkOutcome>> read_asr_chunk_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads) {
  const AsrChunkBatchParameters parameters =
      asr_chunk_batch_parameters_from_json(spec.parameters);
  const nlohmann::json body =
      detail::read_cbor_output(spec, outputs, payloads, kAsrChunkOutcomesRole);
  const nlohmann::json& records = body.at("chunks");
  if (!records.is_array() || records.size() != parameters.chunks.size()) {
    throw std::invalid_argument(spec.task_id + ": output does not hold one record per chunk");
  }
  std::vector<std::optional<svp::audio::AsrChunkOutcome>> outcomes;
  outcomes.reserve(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    const nlohmann::json& record = records[index];
    const std::string where = spec.task_id + " record " + std::to_string(index);
    if (record.at("ordinal").get<std::uint64_t>() != parameters.chunks[index].ordinal) {
      throw std::invalid_argument(where + " is not chunk " +
                                  std::to_string(parameters.chunks[index].ordinal));
    }
    if (record.contains("not_run")) {
      outcomes.emplace_back(std::nullopt);
      continue;
    }
    svp::audio::AsrChunkOutcome outcome;
    outcome.ran = true;
    outcome.alignment_status = record.at("alignment_status").get<std::string>();
    outcome.decoded_word_count = record.at("decoded_word_count").get<std::size_t>();
    for (const nlohmann::json& word : record.at("words")) {
      if (!word.is_array() || word.size() != 6) {
        throw std::invalid_argument(where + " has a malformed word");
      }
      svp::audio::AsrWord decoded;
      decoded.text = word.at(0).get<std::string>();
      decoded.start_us = word.at(1).get<std::int64_t>();
      decoded.end_us = word.at(2).get<std::int64_t>();
      decoded.confidence = word.at(3).get<double>();
      decoded.chunk_ordinal = word.at(4).get<std::int64_t>();
      decoded.timing_source = word.at(5).get<std::string>();
      outcome.words.push_back(std::move(decoded));
    }
    outcomes.emplace_back(std::move(outcome));
  }
  return outcomes;
}

}  // namespace svp::audio::tasks
