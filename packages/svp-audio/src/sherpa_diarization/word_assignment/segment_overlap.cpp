#include "types.hpp"

#include <algorithm>
#include <map>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

SegmentOverlap best_segment_overlap_for_word(
    const AsrWord& word,
    const std::vector<SherpaDiarizationSegment>& segments) {
  const std::int64_t word_duration = word.end_us - word.start_us;
  if (word_duration <= 0) return {};

  std::map<int32_t, std::int64_t> overlap_by_speaker;
  for (const auto& seg : segments) {
    if (seg.speaker_id < 0) continue;
    const std::int64_t seg_start =
        static_cast<std::int64_t>(seg.start_sec * 1000000.0f);
    const std::int64_t seg_end =
        static_cast<std::int64_t>(seg.end_sec * 1000000.0f);
    const std::int64_t overlap = std::min(word.end_us, seg_end) -
                                 std::max(word.start_us, seg_start);
    if (overlap > 0) {
      overlap_by_speaker[seg.speaker_id] += overlap;
    }
  }

  if (overlap_by_speaker.empty()) return {};
  const auto best = std::max_element(
      overlap_by_speaker.begin(), overlap_by_speaker.end(),
      [](const auto& lhs, const auto& rhs) {
        return lhs.second < rhs.second;
      });
  if (best == overlap_by_speaker.end() || best->second <= 0) return {};
  return {
      best->first,
      static_cast<float>(best->second) / static_cast<float>(word_duration)
  };
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
