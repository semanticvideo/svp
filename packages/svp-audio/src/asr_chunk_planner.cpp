#include "svp/audio/asr_chunk_planner.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

std::string chunk_id_for_ordinal(std::size_t ordinal) {
  std::ostringstream output;
  output << "asr_chunk_" << std::setw(6) << std::setfill('0') << ordinal;
  return output.str();
}

std::string chunk_output_ref(std::size_t ordinal) {
  std::ostringstream output;
  output << "transcript/words.chunk_" << std::setw(6) << std::setfill('0')
         << ordinal << ".jsonl";
  return output.str();
}

}  // namespace

AsrChunkPlanResult build_asr_chunk_plan(std::int64_t total_duration_us,
                                        std::int64_t chunk_duration_us,
                                        std::int64_t overlap_us,
                                        const std::string& input_ref,
                                        const std::string& model_id,
                                        const std::string& runtime) {
  AsrChunkPlanResult result;
  result.chunk_duration_us = chunk_duration_us;
  result.overlap_us = overlap_us;
  result.total_duration_us = total_duration_us;

  if (total_duration_us < 0) {
    throw std::invalid_argument("total_duration_us must be non-negative");
  }
  if (chunk_duration_us <= 0) {
    throw std::invalid_argument("chunk_duration_us must be positive");
  }
  if (overlap_us < 0) {
    throw std::invalid_argument("overlap_us must be non-negative");
  }
  if (overlap_us >= chunk_duration_us) {
    throw std::invalid_argument("overlap_us must be less than chunk_duration_us");
  }

  if (total_duration_us == 0) {
    return result;
  }

  const std::int64_t step = chunk_duration_us - overlap_us;
  if (step <= 0) {
    throw std::invalid_argument("effective step (chunk_duration - overlap) must be positive");
  }

  std::int64_t cursor = 0;
  while (cursor < total_duration_us) {
    const std::int64_t chunk_start = cursor;
    const std::int64_t chunk_end =
        std::min(cursor + chunk_duration_us, total_duration_us);

    const std::size_t ordinal = result.chunks.size();
    AsrChunkPlan chunk;
    chunk.chunk_id = chunk_id_for_ordinal(ordinal);
    chunk.source_start_us = chunk_start;
    chunk.source_end_us = chunk_end;
    chunk.input_ref = input_ref;
    chunk.output_ref = chunk_output_ref(ordinal);
    chunk.model_id = model_id;
    chunk.runtime = runtime;
    chunk.asr_status = "planned";

    if (ordinal == 0) {
      chunk.overlap_before_us = 0;
    } else {
      chunk.overlap_before_us = std::min(overlap_us, chunk_start);
    }

    const std::int64_t next_start = cursor + step;
    if (next_start < total_duration_us) {
      chunk.overlap_after_us = std::min(overlap_us, total_duration_us - next_start);
    } else {
      chunk.overlap_after_us = 0;
    }

    result.chunks.push_back(std::move(chunk));
    cursor += step;
  }

  return result;
}

nlohmann::json asr_chunk_to_json(const AsrChunkPlan& chunk) {
  return {
      {"chunk_id", chunk.chunk_id},
      {"source_start_us", chunk.source_start_us},
      {"source_end_us", chunk.source_end_us},
      {"overlap_before_us", chunk.overlap_before_us},
      {"overlap_after_us", chunk.overlap_after_us},
      {"decoder_context_before_us", chunk.overlap_before_us},
      {"input_ref", chunk.input_ref},
      {"output_ref", chunk.output_ref},
      {"model_id", chunk.model_id},
      {"runtime", chunk.runtime},
      {"asr_status", chunk.asr_status},
  };
}

nlohmann::json asr_chunk_plan_to_json(const AsrChunkPlanResult& plan) {
  nlohmann::json chunks = nlohmann::json::array();
  for (const AsrChunkPlan& chunk : plan.chunks) {
    chunks.push_back(asr_chunk_to_json(chunk));
  }

  return {
      {"chunks", chunks},
      {"chunk_count", plan.chunks.size()},
      {"chunk_duration_us", plan.chunk_duration_us},
      {"overlap_us", plan.overlap_us},
      {"total_duration_us", plan.total_duration_us},
      {"blockers", plan.blockers},
  };
}

}  // namespace svp::audio
