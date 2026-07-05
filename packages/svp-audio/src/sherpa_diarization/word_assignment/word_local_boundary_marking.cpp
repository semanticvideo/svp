#include "types.hpp"

#include <algorithm>
#include <cstdlib>

namespace svp::audio::sherpa_diarization_internal::word_assignment {
namespace {

void mark_boundary_window(
    const std::vector<AsrWord>& words,
    AssignmentState& state,
    std::int64_t boundary_us) {
  for (std::size_t i = 0; i < words.size(); ++i) {
    const std::int64_t midpoint_us =
        words[i].start_us + ((words[i].end_us - words[i].start_us) / 2);
    if (std::llabs(midpoint_us - boundary_us) >
        kSelectiveWordLocalBoundaryRadiusUs) {
      continue;
    }
    const std::size_t first =
        i > kSelectiveWordLocalBoundaryRadiusWords
            ? i - kSelectiveWordLocalBoundaryRadiusWords
            : 0;
    const std::size_t last = std::min(
        words.size() - 1, i + kSelectiveWordLocalBoundaryRadiusWords);
    for (std::size_t j = first; j <= last; ++j) {
      state.word_local_required[j] = true;
    }
  }
}

}  // namespace

void mark_word_local_refinement_boundaries(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  for (std::size_t i = 1; i < words.size(); ++i) {
    const int32_t previous_speaker =
        speaker_index_from_id(state.assignments[i - 1],
                              diar_result.final_speaker_count);
    const int32_t speaker =
        speaker_index_from_id(state.assignments[i],
                              diar_result.final_speaker_count);
    if (previous_speaker >= 0 && speaker >= 0 && previous_speaker != speaker) {
      mark_boundary_window(words, state, words[i].start_us);
    }
  }
  if (diar_result.final_speaker_count != 2) return;

  for (std::size_t seg_index = 1; seg_index < diar_result.segments.size();
       ++seg_index) {
    const auto& previous = diar_result.segments[seg_index - 1];
    const auto& segment = diar_result.segments[seg_index];
    if (previous.speaker_id != segment.speaker_id) {
      mark_boundary_window(
          words, state,
          static_cast<std::int64_t>(segment.start_sec * 1000000.0f));
    }
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
