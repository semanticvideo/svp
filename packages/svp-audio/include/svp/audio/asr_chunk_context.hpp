#pragma once

#include "svp/audio/asr_chunk_planner.hpp"

#include <cstdint>
#include <vector>

namespace svp::audio {

struct AsrChunkContextPlan {
  std::int64_t slice_start_us = 0;
  std::int64_t slice_end_us = 0;
  std::int64_t nominal_start_offset_us = 0;
};

[[nodiscard]] AsrChunkContextPlan plan_asr_chunk_context(
    const AsrChunkPlan& chunk);

[[nodiscard]] std::vector<AsrWord> retain_nominal_chunk_words(
    const std::vector<AsrWord>& decoded_words,
    const AsrChunkContextPlan& context,
    const AsrChunkPlan& chunk,
    std::int64_t chunk_ordinal);

}  // namespace svp::audio
