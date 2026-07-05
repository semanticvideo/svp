#include "types.hpp"

#include <algorithm>
#include <limits>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

int32_t best_segment_speaker_for_word(
    const AsrWord& word,
    const std::vector<SherpaDiarizationSegment>& segments) {
  const SherpaDiarizationSegment* best_seg = nullptr;
  std::int64_t best_overlap = 0;
  for (const auto& seg : segments) {
    const std::int64_t seg_start =
        static_cast<std::int64_t>(seg.start_sec * 1000000.0f);
    const std::int64_t seg_end =
        static_cast<std::int64_t>(seg.end_sec * 1000000.0f);
    const std::int64_t overlap = std::min(word.end_us, seg_end) -
                                 std::max(word.start_us, seg_start);
    if (overlap > best_overlap) {
      best_overlap = overlap;
      best_seg = &seg;
    }
  }
  if (best_seg) return best_seg->speaker_id;

  const SherpaDiarizationSegment* nearest_seg = nullptr;
  std::int64_t nearest_dist = std::numeric_limits<std::int64_t>::max();
  for (const auto& seg : segments) {
    const std::int64_t seg_start =
        static_cast<std::int64_t>(seg.start_sec * 1000000.0f);
    const std::int64_t seg_end =
        static_cast<std::int64_t>(seg.end_sec * 1000000.0f);
    std::int64_t dist = 0;
    if (word.end_us <= seg_start) {
      dist = seg_start - word.end_us;
    } else if (word.start_us >= seg_end) {
      dist = word.start_us - seg_end;
    }
    if (dist < nearest_dist) {
      nearest_dist = dist;
      nearest_seg = &seg;
    }
  }
  if (nearest_seg && nearest_dist <= kWordSpeakerNearestToleranceUs) {
    return nearest_seg->speaker_id;
  }
  return -1;
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
