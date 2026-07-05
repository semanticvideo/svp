#include "types.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

int32_t dominant_speaker_from_segments(
    const std::vector<SherpaDiarizationSegment>& segments,
    int32_t speaker_count) {
  std::vector<float> durations(static_cast<std::size_t>(speaker_count), 0.0f);
  for (const auto& seg : segments) {
    if (seg.speaker_id < 0 || seg.speaker_id >= speaker_count) continue;
    durations[static_cast<std::size_t>(seg.speaker_id)] +=
        std::max(0.0f, seg.end_sec - seg.start_sec);
  }

  int32_t dominant = -1;
  float dominant_duration = 0.0f;
  for (int32_t speaker = 0; speaker < speaker_count; ++speaker) {
    const float duration = durations[static_cast<std::size_t>(speaker)];
    if (duration > dominant_duration) {
      dominant = speaker;
      dominant_duration = duration;
    }
  }
  return dominant;
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
