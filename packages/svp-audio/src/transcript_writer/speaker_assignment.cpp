#include "internal.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace svp::audio::transcript_writer_internal {
namespace {

constexpr std::int64_t kSpeakerAssignmentToleranceUs = 500000;
constexpr std::int64_t kSpeakerUtteranceGapThresholdUs = 750000;
constexpr std::size_t kSpeakerUtteranceExpansionMinWords = 8;

bool ends_utterance(const std::string& text) {
  if (text.empty()) return false;
  const char last = text.back();
  return last == '.' || last == '?' || last == '!';
}

std::string assign_speaker_by_segment_overlap(
    const AsrWord& word,
    const std::vector<SpeakerSegment>& speaker_segments) {
  const SpeakerSegment* best_seg = nullptr;
  std::int64_t best_overlap = 0;
  for (const SpeakerSegment& seg : speaker_segments) {
    const std::int64_t overlap = std::min(word.end_us, seg.timing.end_us) -
                                 std::max(word.start_us, seg.timing.start_us);
    if (overlap > best_overlap) {
      best_overlap = overlap;
      best_seg = &seg;
    }
  }
  if (best_seg) {
    return best_seg->speaker_id;
  }

  const SpeakerSegment* nearest_seg = nullptr;
  std::int64_t nearest_dist = std::numeric_limits<std::int64_t>::max();
  for (const SpeakerSegment& seg : speaker_segments) {
    std::int64_t dist;
    if (word.end_us <= seg.timing.start_us) {
      dist = seg.timing.start_us - word.end_us;
    } else if (word.start_us >= seg.timing.end_us) {
      dist = word.start_us - seg.timing.end_us;
    } else {
      dist = 0;
    }
    if (dist < nearest_dist) {
      nearest_dist = dist;
      nearest_seg = &seg;
    }
  }
  if (nearest_seg && nearest_dist <= kSpeakerAssignmentToleranceUs) {
    return nearest_seg->speaker_id;
  }
  return "speaker_unknown";
}

std::string dominant_segment_speaker(
    const std::vector<SpeakerSegment>& speaker_segments) {
  std::map<std::string, std::int64_t> durations;
  for (const SpeakerSegment& seg : speaker_segments) {
    durations[seg.speaker_id] += std::max<std::int64_t>(
        0, seg.timing.end_us - seg.timing.start_us);
  }

  std::string dominant;
  std::int64_t dominant_duration = 0;
  for (const auto& [speaker_id, duration] : durations) {
    if (duration > dominant_duration) {
      dominant = speaker_id;
      dominant_duration = duration;
    }
  }
  return dominant;
}

void expand_sustained_non_dominant_utterance_speakers(
    const std::vector<AsrWord>& words,
    const std::vector<SpeakerSegment>& speaker_segments,
    std::vector<std::string>& assignments) {
  if (words.size() != assignments.size() || words.empty() ||
      speaker_segments.empty()) {
    return;
  }

  const std::string dominant_speaker = dominant_segment_speaker(speaker_segments);
  if (dominant_speaker.empty()) return;

  auto smooth_group = [&](std::size_t first_word, std::size_t last_word) {
    std::map<std::string, std::size_t> counts;
    for (std::size_t i = first_word; i <= last_word && i < assignments.size(); ++i) {
      if (assignments[i] != "speaker_unknown") {
        ++counts[assignments[i]];
      }
    }

    std::string candidate;
    std::size_t candidate_count = 0;
    bool tied = false;
    for (const auto& [speaker_id, count] : counts) {
      if (speaker_id == dominant_speaker ||
          count < kSpeakerUtteranceExpansionMinWords) {
        continue;
      }
      if (count > candidate_count) {
        candidate = speaker_id;
        candidate_count = count;
        tied = false;
      } else if (count == candidate_count) {
        tied = true;
      }
    }

    if (candidate.empty() || tied) return;
    for (std::size_t i = first_word; i <= last_word && i < assignments.size(); ++i) {
      assignments[i] = candidate;
    }
  };

  std::size_t group_start = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const bool last_word = i + 1 == words.size();
    const bool gap_after =
        !last_word &&
        words[i + 1].start_us - words[i].end_us > kSpeakerUtteranceGapThresholdUs;
    if (last_word || gap_after || ends_utterance(words[i].text)) {
      smooth_group(group_start, i);
      group_start = i + 1;
    }
  }
}

}  // namespace

std::vector<std::string> assign_word_speakers(
    const AsrExecutionBoundary& boundary) {
  std::vector<std::string> assignments;
  assignments.reserve(boundary.reconciled_words.size());

  const std::string sole_segment_speaker =
      boundary.speaker_count == 1 && !boundary.speaker_segments.empty()
          ? dominant_segment_speaker(boundary.speaker_segments)
          : "";

  bool used_external_assignments = false;
  for (std::size_t i = 0; i < boundary.reconciled_words.size(); ++i) {
    const AsrWord& word = boundary.reconciled_words[i];
    std::string speaker_id = "speaker_unknown";
    if (i < boundary.word_speaker_assignments.size() &&
        !boundary.word_speaker_assignments[i].empty()) {
      speaker_id = boundary.word_speaker_assignments[i];
      used_external_assignments = true;
    } else if (boundary.one_speaker_mode) {
      speaker_id = "speaker_0001";
    } else if (!boundary.speaker_segments.empty()) {
      speaker_id = assign_speaker_by_segment_overlap(word, boundary.speaker_segments);
      if (speaker_id == "speaker_unknown" && !sole_segment_speaker.empty()) {
        speaker_id = sole_segment_speaker;
      }
    }
    assignments.push_back(std::move(speaker_id));
  }

  if (!used_external_assignments && !boundary.one_speaker_mode &&
      boundary.speaker_count > 1) {
    expand_sustained_non_dominant_utterance_speakers(
        boundary.reconciled_words, boundary.speaker_segments, assignments);
  }

  return assignments;
}

}  // namespace svp::audio::transcript_writer_internal
