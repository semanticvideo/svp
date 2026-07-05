#include "types.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void repair_delayed_segment_handoffs(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  for (std::size_t seg_index = 1; seg_index < diar_result.segments.size();
       ++seg_index) {
    const auto& previous = diar_result.segments[seg_index - 1];
    const auto& segment = diar_result.segments[seg_index];
    if (previous.speaker_id == segment.speaker_id ||
        segment.speaker_id < 0 ||
        segment.speaker_id >= diar_result.final_speaker_count) {
      continue;
    }

    const std::int64_t segment_start_us =
        static_cast<std::int64_t>(segment.start_sec * 1000000.0f);
    const std::string segment_speaker_id =
        speaker_id_for_index(segment.speaker_id);

    std::size_t first_word = words.size();
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::int64_t midpoint_us =
          words[i].start_us + ((words[i].end_us - words[i].start_us) / 2);
      if (midpoint_us >= segment_start_us &&
          midpoint_us - segment_start_us <=
              kFingerprintDelayedHandoffMaxDelayUs) {
        first_word = i;
        break;
      }
    }
    if (first_word == words.size() ||
        state.assignments[first_word] == segment_speaker_id) {
      continue;
    }

    std::size_t natural_switch = words.size();
    std::int64_t previous_end_us = words[first_word].end_us;
    const std::size_t search_end = std::min(
        words.size(),
        first_word + kFingerprintDelayedHandoffMaxWords + 2);
    for (std::size_t i = first_word + 1; i < search_end; ++i) {
      if (words[i].start_us - previous_end_us > kUtteranceGapThresholdUs) {
        break;
      }
      if (state.assignments[i] == segment_speaker_id) {
        natural_switch = i;
        break;
      }
      previous_end_us = words[i].end_us;
    }
    if (natural_switch == words.size()) continue;

    for (std::size_t i = first_word; i < natural_switch; ++i) {
      state.assignments[i] = segment_speaker_id;
    }
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
