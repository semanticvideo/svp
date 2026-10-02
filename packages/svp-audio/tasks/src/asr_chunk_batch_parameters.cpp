#include "svp/audio/tasks/asr_chunk_batch.hpp"

#include "json_fields.hpp"

#include <stdexcept>

namespace svp::audio::tasks {
namespace {

nlohmann::json chunk_json(const AsrChunkBatchItem& item) {
  const svp::audio::AsrChunkPlan& chunk = item.chunk;
  return {{"asr_status", chunk.asr_status},
          {"chunk_id", chunk.chunk_id},
          {"input_ref", chunk.input_ref},
          {"model_id", chunk.model_id},
          {"ordinal", item.ordinal},
          {"output_ref", chunk.output_ref},
          {"overlap_after_us", chunk.overlap_after_us},
          {"overlap_before_us", chunk.overlap_before_us},
          {"runtime", chunk.runtime},
          {"source_end_us", chunk.source_end_us},
          {"source_start_us", chunk.source_start_us}};
}

AsrChunkBatchItem chunk_from_json(const nlohmann::json& value, const std::string& where) {
  detail::require_only(value,
                       {"asr_status", "chunk_id", "input_ref", "model_id", "ordinal",
                        "output_ref", "overlap_after_us", "overlap_before_us", "runtime",
                        "source_end_us", "source_start_us"},
                       where);
  AsrChunkBatchItem item;
  item.ordinal = detail::required<std::uint64_t>(value, "ordinal", where);
  item.chunk.asr_status = detail::required<std::string>(value, "asr_status", where);
  item.chunk.chunk_id = detail::required<std::string>(value, "chunk_id", where);
  item.chunk.input_ref = detail::required<std::string>(value, "input_ref", where);
  item.chunk.model_id = detail::required<std::string>(value, "model_id", where);
  item.chunk.output_ref = detail::required<std::string>(value, "output_ref", where);
  item.chunk.overlap_after_us = detail::required<std::int64_t>(value, "overlap_after_us", where);
  item.chunk.overlap_before_us = detail::required<std::int64_t>(value, "overlap_before_us", where);
  item.chunk.runtime = detail::required<std::string>(value, "runtime", where);
  item.chunk.source_end_us = detail::required<std::int64_t>(value, "source_end_us", where);
  item.chunk.source_start_us = detail::required<std::int64_t>(value, "source_start_us", where);
  if (item.chunk.source_end_us <= item.chunk.source_start_us) {
    throw std::invalid_argument(where + ": source_end_us must follow source_start_us");
  }
  return item;
}

}  // namespace

nlohmann::json asr_chunk_batch_parameters_to_json(const AsrChunkBatchParameters& parameters) {
  nlohmann::json chunks = nlohmann::json::array();
  for (const AsrChunkBatchItem& item : parameters.chunks) {
    chunks.push_back(chunk_json(item));
  }
  nlohmann::json value = {
      {"alignment_model_id", parameters.alignment_model_id
                                 ? nlohmann::json(*parameters.alignment_model_id)
                                 : nlohmann::json(nullptr)},
      {"chunks", std::move(chunks)},
      {"model_id", parameters.model_id},
      {"threads",
       {{"alignment", detail::ort_threads_json(parameters.threads.forced_alignment)},
        {"decode", parameters.threads.whisper.decode},
        {"vad", parameters.threads.whisper.vad}}},
      {"vad_model_id", parameters.vad_model_id},
  };
  if (const std::optional<std::string> problem = validate_asr_chunk_batch_parameters(value)) {
    throw std::invalid_argument("asr.chunk_batch parameters: " + *problem);
  }
  return value;
}

AsrChunkBatchParameters asr_chunk_batch_parameters_from_json(const nlohmann::json& value) {
  const std::string where = "asr.chunk_batch parameters";
  detail::require_only(value, {"alignment_model_id", "chunks", "model_id", "threads",
                               "vad_model_id"},
                       where);
  AsrChunkBatchParameters parameters;
  const nlohmann::json& alignment = value.at("alignment_model_id");
  if (!alignment.is_null()) {
    if (!alignment.is_string() || alignment.get<std::string>().empty()) {
      throw std::invalid_argument(where + ": alignment_model_id must be null or a model ID");
    }
    parameters.alignment_model_id = alignment.get<std::string>();
  }
  parameters.model_id = detail::required<std::string>(value, "model_id", where);
  parameters.vad_model_id = detail::required<std::string>(value, "vad_model_id", where);
  if (parameters.model_id.empty() || parameters.vad_model_id.empty()) {
    throw std::invalid_argument(where + ": model IDs must not be empty");
  }
  const nlohmann::json& threads = value.at("threads");
  detail::require_only(threads, {"alignment", "decode", "vad"}, where + ".threads");
  parameters.threads.whisper.decode = detail::required<int>(threads, "decode", where);
  parameters.threads.whisper.vad = detail::required<int>(threads, "vad", where);
  parameters.threads.forced_alignment =
      detail::ort_threads_from_json(threads.at("alignment"), where + ".threads.alignment");
  if (parameters.threads.whisper.decode < 1 || parameters.threads.whisper.vad < 1) {
    throw std::invalid_argument(where + ": whisper thread counts must be at least 1");
  }
  const nlohmann::json& chunks = value.at("chunks");
  if (!chunks.is_array() || chunks.empty()) {
    throw std::invalid_argument(where + ": chunks must be a non-empty array");
  }
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    AsrChunkBatchItem item =
        chunk_from_json(chunks[index], where + ".chunks[" + std::to_string(index) + "]");
    if (!parameters.chunks.empty() && item.ordinal <= parameters.chunks.back().ordinal) {
      throw std::invalid_argument(where + ": chunk ordinals must ascend");
    }
    parameters.chunks.push_back(std::move(item));
  }
  return parameters;
}

std::optional<std::string> validate_asr_chunk_batch_parameters(const nlohmann::json& value) {
  try {
    (void)asr_chunk_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::exception& error) {
    return std::string(error.what());
  }
}

}  // namespace svp::audio::tasks
