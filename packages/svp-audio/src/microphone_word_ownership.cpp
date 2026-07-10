#include "svp/audio/microphone_transcript.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace svp::audio {
namespace {

struct OwnershipTurn {
  std::int64_t chunk_ordinal = 0;
  TimeSpan timing;
};

std::string normalized_token(const std::string& text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (unsigned char character : text) {
    if (std::isalnum(character)) {
      normalized.push_back(static_cast<char>(std::tolower(character)));
    }
  }
  return normalized;
}

std::size_t lcs_length(const std::vector<std::string>& left,
                       const std::vector<std::string>& right) {
  std::vector<std::size_t> previous(right.size() + 1, 0);
  std::vector<std::size_t> current(right.size() + 1, 0);
  for (const std::string& left_token : left) {
    for (std::size_t right_index = 0; right_index < right.size();
         ++right_index) {
      if (left_token == right[right_index]) {
        current[right_index + 1] = previous[right_index] + 1;
      } else {
        current[right_index + 1] =
            std::max(previous[right_index + 1], current[right_index]);
      }
    }
    std::swap(previous, current);
    std::fill(current.begin(), current.end(), 0);
  }
  return previous.back();
}

std::vector<std::int64_t> shared_chunk_ordinals(
    const std::map<std::int64_t, std::vector<std::string>>& left,
    const std::map<std::int64_t, std::vector<std::string>>& right) {
  std::vector<std::int64_t> left_chunks;
  std::vector<std::int64_t> right_chunks;
  for (const auto& [chunk, tokens] : left) {
    if (!tokens.empty()) left_chunks.push_back(chunk);
  }
  for (const auto& [chunk, tokens] : right) {
    if (!tokens.empty()) right_chunks.push_back(chunk);
  }
  std::vector<std::int64_t> shared;
  std::set_intersection(left_chunks.begin(), left_chunks.end(),
                        right_chunks.begin(), right_chunks.end(),
                        std::back_inserter(shared));
  return shared;
}

std::optional<double> local_snr_db(const MicrophoneTranscript& transcript,
                                   const TimeSpan& timing) {
  if (!transcript.signal_profile.noise_floor_db.has_value()) return std::nullopt;
  std::vector<double> levels;
  for (const MicrophoneSignalFrame& frame : transcript.signal_profile.frames) {
    if (frame.timing.start_us >= timing.end_us) break;
    if (std::min(frame.timing.end_us, timing.end_us) >
        std::max(frame.timing.start_us, timing.start_us)) {
      levels.push_back(frame.signal_db);
    }
  }
  if (levels.empty()) return std::nullopt;
  std::sort(levels.begin(), levels.end());
  const std::size_t middle = levels.size() / 2;
  const double median = (levels.size() & 1U) != 0U
                            ? levels[middle]
                            : (levels[middle - 1] + levels[middle]) / 2.0;
  return median - *transcript.signal_profile.noise_floor_db;
}

bool terminal_punctuation(const std::string& text) {
  for (auto iterator = text.rbegin(); iterator != text.rend(); ++iterator) {
    const unsigned char character = static_cast<unsigned char>(*iterator);
    if (std::isspace(character)) continue;
    return character == '.' || character == '?' || character == '!';
  }
  return false;
}

std::vector<OwnershipTurn> build_ownership_turns(
    const MicrophoneTranscript& transcript,
    const MicrophoneDeduplicationPolicy& policy) {
  std::vector<const AsrWord*> ordered_words;
  ordered_words.reserve(transcript.words.size());
  for (const AsrWord& word : transcript.words) ordered_words.push_back(&word);
  std::sort(ordered_words.begin(), ordered_words.end(),
            [](const AsrWord* left, const AsrWord* right) {
              if (left->chunk_ordinal != right->chunk_ordinal) {
                return left->chunk_ordinal < right->chunk_ordinal;
              }
              return left->start_us < right->start_us;
            });

  std::vector<OwnershipTurn> turns;
  const AsrWord* previous = nullptr;
  for (const AsrWord* word : ordered_words) {
    const bool begins_turn =
        previous == nullptr ||
        word->chunk_ordinal != previous->chunk_ordinal ||
        terminal_punctuation(previous->text) ||
        word->start_us - previous->end_us >
            policy.maximum_speaker_segment_gap_us;
    if (begins_turn) {
      turns.push_back(
          {word->chunk_ordinal, {word->start_us, word->end_us}});
    } else {
      turns.back().timing.end_us =
          std::max(turns.back().timing.end_us, word->end_us);
    }
    previous = word;
  }
  return turns;
}

TimeSpan ownership_timing(const std::vector<OwnershipTurn>& turns,
                          const AsrWord& word) {
  const OwnershipTurn* best_turn = nullptr;
  std::int64_t best_overlap_us = 0;
  for (const OwnershipTurn& turn : turns) {
    if (turn.chunk_ordinal != word.chunk_ordinal) continue;
    const std::int64_t overlap_us = std::max<std::int64_t>(
        0, std::min(turn.timing.end_us, word.end_us) -
               std::max(turn.timing.start_us, word.start_us));
    if (overlap_us > best_overlap_us) {
      best_overlap_us = overlap_us;
      best_turn = &turn;
    }
  }
  return best_turn == nullptr ? TimeSpan{word.start_us, word.end_us}
                              : best_turn->timing;
}

}  // namespace

MicrophoneWordOwnershipResult reconcile_cross_anchor_word_ownership(
    const std::vector<MicrophoneOwnedWordCandidate>& words,
    const std::vector<MicrophoneTranscript>& transcripts,
    const std::vector<MicrophoneSpeakerAnchor>& anchors,
    const MicrophoneDeduplicationPolicy& policy) {
  MicrophoneWordOwnershipResult result;
  std::map<std::size_t, std::size_t> transcript_by_group;
  std::map<std::size_t, std::vector<OwnershipTurn>> turns_by_group;
  for (const MicrophoneSpeakerAnchor& anchor : anchors) {
    transcript_by_group[anchor.voice_group] = anchor.transcript_index;
    turns_by_group[anchor.voice_group] = build_ownership_turns(
        transcripts.at(anchor.transcript_index), policy);
  }

  std::map<std::size_t,
           std::map<std::int64_t, std::vector<std::string>>>
      candidate_tokens_by_group;
  for (const MicrophoneOwnedWordCandidate& candidate : words) {
    std::string token = normalized_token(candidate.word.text);
    if (!token.empty()) {
      candidate_tokens_by_group[candidate.voice_group]
                               [candidate.word.chunk_ordinal]
                                   .push_back(std::move(token));
    }
  }

  std::map<std::pair<std::size_t, std::size_t>,
           std::map<std::int64_t, bool>> duplicate_chunks;
  for (std::size_t left_index = 0; left_index < anchors.size(); ++left_index) {
    const MicrophoneSpeakerAnchor& left_anchor = anchors[left_index];
    const MicrophoneTranscript& left =
        transcripts.at(left_anchor.transcript_index);
    for (std::size_t right_index = left_index + 1;
         right_index < anchors.size(); ++right_index) {
      const MicrophoneSpeakerAnchor& right_anchor = anchors[right_index];
      const MicrophoneTranscript& right =
          transcripts.at(right_anchor.transcript_index);
      const auto group_pair = std::make_pair(
          std::min(left_anchor.voice_group, right_anchor.voice_group),
          std::max(left_anchor.voice_group, right_anchor.voice_group));
      const auto& left_group_tokens =
          candidate_tokens_by_group[left_anchor.voice_group];
      const auto& right_group_tokens =
          candidate_tokens_by_group[right_anchor.voice_group];
      for (const std::int64_t chunk_ordinal : shared_chunk_ordinals(
               left_group_tokens, right_group_tokens)) {
        const std::vector<std::string>& left_tokens =
            left_group_tokens.at(chunk_ordinal);
        const std::vector<std::string>& right_tokens =
            right_group_tokens.at(chunk_ordinal);
        const std::size_t aligned = lcs_length(left_tokens, right_tokens);
        const std::size_t token_total = left_tokens.size() + right_tokens.size();
        const double ratio = token_total > 0
                                 ? 2.0 * static_cast<double>(aligned) /
                                       static_cast<double>(token_total)
                                 : 0.0;
        const bool duplicate =
            ratio > policy.minimum_duplicate_chunk_token_alignment_ratio;
        duplicate_chunks[group_pair][chunk_ordinal] = duplicate;
        result.chunk_content_evidence.push_back({
            left.source_ordinal,
            right.source_ordinal,
            chunk_ordinal,
            left_tokens.size(),
            right_tokens.size(),
            aligned,
            ratio,
            duplicate,
        });
      }
    }
  }

  for (const MicrophoneOwnedWordCandidate& candidate : words) {
    const auto own_anchor = transcript_by_group.find(candidate.voice_group);
    if (own_anchor == transcript_by_group.end()) continue;
    const MicrophoneTranscript& own_transcript =
        transcripts.at(own_anchor->second);
    const TimeSpan comparison_timing = ownership_timing(
        turns_by_group.at(candidate.voice_group), candidate.word);
    const std::optional<double> own_snr =
        local_snr_db(own_transcript, comparison_timing);
    bool loses_duplicate_capture = false;
    for (const MicrophoneSpeakerAnchor& other_anchor : anchors) {
      if (other_anchor.voice_group == candidate.voice_group) continue;
      const auto group_pair = std::make_pair(
          std::min(candidate.voice_group, other_anchor.voice_group),
          std::max(candidate.voice_group, other_anchor.voice_group));
      const auto pair_found = duplicate_chunks.find(group_pair);
      if (pair_found == duplicate_chunks.end()) continue;
      const auto chunk_found =
          pair_found->second.find(candidate.word.chunk_ordinal);
      if (chunk_found == pair_found->second.end() || !chunk_found->second) {
        continue;
      }
      const MicrophoneTranscript& other_transcript =
          transcripts.at(other_anchor.transcript_index);
      const std::optional<double> other_snr =
          local_snr_db(other_transcript, comparison_timing);
      if (own_snr.has_value() && other_snr.has_value() &&
          *other_snr > *own_snr) {
        loses_duplicate_capture = true;
        break;
      }
    }
    if (loses_duplicate_capture) {
      ++result.discarded_cross_anchor_bleed_word_count;
    } else {
      result.words.push_back(candidate);
    }
  }
  return result;
}

}  // namespace svp::audio
