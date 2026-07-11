#include "svp/audio/asr_chunk_context.hpp"

#include <algorithm>

namespace svp::audio {

AsrChunkContextPlan plan_asr_chunk_context(const AsrChunkPlan& chunk) {
  const std::int64_t context_before_us =
      std::min(chunk.overlap_before_us, chunk.source_start_us);
  return {
      chunk.source_start_us - context_before_us,
      chunk.source_end_us,
      context_before_us,
  };
}

std::vector<AsrWord> retain_nominal_chunk_words(
    const std::vector<AsrWord>& decoded_words,
    const AsrChunkContextPlan& context,
    const AsrChunkPlan& chunk,
    std::int64_t chunk_ordinal) {
  const std::int64_t nominal_duration_us =
      chunk.source_end_us - chunk.source_start_us;
  const std::int64_t nominal_end_us =
      context.nominal_start_offset_us + nominal_duration_us;

  std::vector<AsrWord> retained;
  for (const AsrWord& word : decoded_words) {
    const std::int64_t center_us =
        word.start_us + (word.end_us - word.start_us) / 2;
    if (center_us < context.nominal_start_offset_us ||
        center_us >= nominal_end_us) {
      continue;
    }

    AsrWord adjusted = word;
    adjusted.start_us = std::max<std::int64_t>(
        0, word.start_us - context.nominal_start_offset_us);
    adjusted.end_us = std::min(
        nominal_duration_us, word.end_us - context.nominal_start_offset_us);
    adjusted.chunk_ordinal = chunk_ordinal;
    if (adjusted.end_us > adjusted.start_us) {
      retained.push_back(std::move(adjusted));
    }
  }
  return retained;
}

}  // namespace svp::audio
