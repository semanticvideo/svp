#include "types.hpp"

namespace svp::audio::sherpa_diarization_internal::word_assignment {
namespace {

bool has_clause_or_utterance_punctuation(const std::string& text) {
  if (text.empty()) return false;
  const char last = text.back();
  return last == '.' || last == '?' || last == '!' || last == ',' ||
         last == ';' || last == ':';
}

}  // namespace

void repair_punctuated_segment_islands(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  std::size_t run_start = 0;
  while (run_start < state.assignments.size()) {
    std::size_t run_end = run_start;
    while (run_end + 1 < state.assignments.size() &&
           state.assignments[run_end + 1] == state.assignments[run_start]) {
      ++run_end;
    }

    const std::size_t run_words = run_end - run_start + 1;
    if (run_words <= kFingerprintPunctuatedIslandMaxWords) {
      bool has_punctuation = false;
      for (std::size_t i = run_start; i <= run_end; ++i) {
        if (has_clause_or_utterance_punctuation(words[i].text)) {
          has_punctuation = true;
          break;
        }
      }

      if (has_punctuation) {
        int32_t segment_speaker = -1;
        bool all_words_match_segment = true;
        for (std::size_t i = run_start; i <= run_end; ++i) {
          if (state.group_decision_supported[i]) {
            all_words_match_segment = false;
            break;
          }
          const SegmentOverlap overlap =
              best_segment_overlap_for_word(words[i], diar_result.segments);
          if (overlap.speaker < 0 ||
              overlap.overlap_fraction <
                  kFingerprintPunctuatedIslandMinSegmentOverlap ||
              speaker_index_from_id(state.assignments[i],
                                    diar_result.final_speaker_count) ==
                  overlap.speaker) {
            all_words_match_segment = false;
            break;
          }
          if (segment_speaker < 0) {
            segment_speaker = overlap.speaker;
          } else if (segment_speaker != overlap.speaker) {
            all_words_match_segment = false;
            break;
          }
        }

        if (all_words_match_segment && segment_speaker >= 0) {
          const std::string segment_speaker_id =
              speaker_id_for_index(segment_speaker);
          for (std::size_t i = run_start; i <= run_end; ++i) {
            state.assignments[i] = segment_speaker_id;
          }
        }
      }
    }

    run_start = run_end + 1;
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
