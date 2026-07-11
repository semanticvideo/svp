#include "svp/audio/microphone_transcript.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
  std::vector<const AsrWord*> words;
  std::vector<std::string> tokens;
};

struct DuplicateTurnMatch {
  std::size_t left_group = 0;
  std::size_t right_group = 0;
  OwnershipTurn left;
  OwnershipTurn right;
  std::vector<std::string> aligned_tokens;
  bool duplicate_capture_proven = false;
};

struct SuppressionContinuation {
  std::size_t weaker_group = 0;
  std::size_t stronger_group = 0;
  std::int64_t chunk_ordinal = 0;
  std::int64_t current_end_us = 0;
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

std::vector<std::string> lcs_tokens(const std::vector<std::string>& left,
                                    const std::vector<std::string>& right) {
  std::vector<std::vector<std::size_t>> lengths(
      left.size() + 1, std::vector<std::size_t>(right.size() + 1, 0));
  for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
    for (std::size_t right_index = 0; right_index < right.size();
         ++right_index) {
      lengths[left_index + 1][right_index + 1] =
          left[left_index] == right[right_index]
              ? lengths[left_index][right_index] + 1
              : std::max(lengths[left_index][right_index + 1],
                         lengths[left_index + 1][right_index]);
    }
  }

  std::vector<std::string> aligned;
  std::size_t left_index = left.size();
  std::size_t right_index = right.size();
  while (left_index > 0 && right_index > 0) {
    if (left[left_index - 1] == right[right_index - 1]) {
      aligned.push_back(left[left_index - 1]);
      --left_index;
      --right_index;
    } else if (lengths[left_index - 1][right_index] >=
               lengths[left_index][right_index - 1]) {
      --left_index;
    } else {
      --right_index;
    }
  }
  std::reverse(aligned.begin(), aligned.end());
  return aligned;
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

double mean_transcript_confidence(const MicrophoneTranscript& transcript) {
  if (transcript.words.empty()) return 0.0;
  double total = 0.0;
  for (const AsrWord& word : transcript.words) total += word.confidence;
  return total / static_cast<double>(transcript.words.size());
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
          {word->chunk_ordinal, {word->start_us, word->end_us}, {}, {}});
    } else {
      turns.back().timing.end_us =
          std::max(turns.back().timing.end_us, word->end_us);
    }
    turns.back().words.push_back(word);
    std::string token = normalized_token(word->text);
    if (!token.empty()) turns.back().tokens.push_back(std::move(token));
    previous = word;
  }
  return turns;
}

std::int64_t overlap_us(const TimeSpan& left, const TimeSpan& right) {
  return std::max<std::int64_t>(
      0, std::min(left.end_us, right.end_us) -
             std::max(left.start_us, right.start_us));
}

bool turns_are_time_aligned(const OwnershipTurn& left,
                            const OwnershipTurn& right,
                            const MicrophoneDeduplicationPolicy& policy) {
  if (left.chunk_ordinal != right.chunk_ordinal) return false;
  return left.timing.start_us <=
             right.timing.end_us + policy.maximum_duplicate_word_time_delta_us &&
         right.timing.start_us <=
             left.timing.end_us + policy.maximum_duplicate_word_time_delta_us;
}

std::int64_t track_overlap_us(const MicrophoneVoiceTrack& track,
                              const TimeSpan& timing) {
  std::int64_t total = 0;
  for (const TimeSpan& segment : track.speech_segments) {
    if (segment.start_us >= timing.end_us) break;
    total += overlap_us(segment, timing);
  }
  return total;
}

const MicrophoneVoiceTrack* dominant_track_at_timing(
    const MicrophoneTranscript& transcript,
    const TimeSpan& timing) {
  const MicrophoneVoiceTrack* best = nullptr;
  std::int64_t best_overlap_us = 0;
  bool tied = false;
  for (const MicrophoneVoiceTrack& track : transcript.voice_tracks) {
    const std::int64_t candidate_overlap_us =
        track_overlap_us(track, timing);
    if (candidate_overlap_us > best_overlap_us) {
      best = &track;
      best_overlap_us = candidate_overlap_us;
      tied = false;
    } else if (candidate_overlap_us > 0 &&
               candidate_overlap_us == best_overlap_us) {
      tied = true;
    }
  }
  return tied ? nullptr : best;
}

double fingerprint_similarity(const std::vector<float>& left,
                              const std::vector<float>& right) {
  if (left.empty() || left.size() != right.size()) return -1.0;
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    dot += static_cast<double>(left[index]) * right[index];
    left_norm += static_cast<double>(left[index]) * left[index];
    right_norm += static_cast<double>(right[index]) * right[index];
  }
  if (left_norm <= 0.0 || right_norm <= 0.0) return -1.0;
  return dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
}

bool local_fingerprint_not_contrary(
    const MicrophoneTranscript& left,
    const MicrophoneTranscript& right,
    const TimeSpan& left_timing,
    const TimeSpan& right_timing,
    const MicrophoneDeduplicationPolicy& policy) {
  const MicrophoneVoiceTrack* left_track =
      dominant_track_at_timing(left, left_timing);
  const MicrophoneVoiceTrack* right_track =
      dominant_track_at_timing(right, right_timing);
  if (left_track == nullptr || right_track == nullptr) return false;
  return fingerprint_similarity(left_track->fingerprint,
                                right_track->fingerprint) >
         policy.maximum_contrary_voice_fingerprint_similarity;
}

bool local_fingerprint_matches(
    const MicrophoneTranscript& left,
    const MicrophoneTranscript& right,
    const TimeSpan& left_timing,
    const TimeSpan& right_timing,
    const MicrophoneDeduplicationPolicy& policy) {
  const MicrophoneVoiceTrack* left_track =
      dominant_track_at_timing(left, left_timing);
  const MicrophoneVoiceTrack* right_track =
      dominant_track_at_timing(right, right_timing);
  if (left_track == nullptr || right_track == nullptr) return false;
  return fingerprint_similarity(left_track->fingerprint,
                                right_track->fingerprint) >=
         policy.minimum_voice_fingerprint_similarity;
}

bool word_in_turn(const AsrWord& word, const OwnershipTurn& turn) {
  return word.chunk_ordinal == turn.chunk_ordinal &&
         word.start_us >= turn.timing.start_us &&
         word.end_us <= turn.timing.end_us;
}

std::vector<std::string> tokens_in_window(
    const MicrophoneTranscript& transcript,
    std::int64_t chunk_ordinal,
    const TimeSpan& window) {
  std::vector<std::string> tokens;
  for (const AsrWord& word : transcript.words) {
    if (word.chunk_ordinal != chunk_ordinal ||
        word.start_us >= window.end_us || word.end_us <= window.start_us) {
      continue;
    }
    std::string token = normalized_token(word.text);
    if (!token.empty()) tokens.push_back(std::move(token));
  }
  return tokens;
}

const AsrWord* aligned_word(const DuplicateTurnMatch& match,
                            const AsrWord& own_word,
                            bool own_is_left,
                            const MicrophoneDeduplicationPolicy& policy) {
  const std::string token = normalized_token(own_word.text);
  if (token.empty() ||
      std::find(match.aligned_tokens.begin(), match.aligned_tokens.end(), token) ==
          match.aligned_tokens.end()) {
    return nullptr;
  }
  const OwnershipTurn& other_turn = own_is_left ? match.right : match.left;
  const AsrWord* closest = nullptr;
  std::int64_t closest_delta_us = policy.maximum_duplicate_word_time_delta_us + 1;
  for (const AsrWord* other_word : other_turn.words) {
    if (normalized_token(other_word->text) != token) continue;
    const std::int64_t delta_us =
        std::abs(other_word->start_us - own_word.start_us);
    if (delta_us <= policy.maximum_duplicate_word_time_delta_us &&
        delta_us < closest_delta_us) {
      closest = other_word;
      closest_delta_us = delta_us;
    }
  }
  return closest;
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

  std::vector<DuplicateTurnMatch> matches;
  for (std::size_t left_index = 0; left_index < anchors.size(); ++left_index) {
    const MicrophoneSpeakerAnchor& left_anchor = anchors[left_index];
    const MicrophoneTranscript& left =
        transcripts.at(left_anchor.transcript_index);
    for (std::size_t right_index = left_index + 1;
         right_index < anchors.size(); ++right_index) {
      const MicrophoneSpeakerAnchor& right_anchor = anchors[right_index];
      const MicrophoneTranscript& right =
          transcripts.at(right_anchor.transcript_index);
      for (const OwnershipTurn& left_turn :
           turns_by_group.at(left_anchor.voice_group)) {
        for (const OwnershipTurn& right_turn :
             turns_by_group.at(right_anchor.voice_group)) {
          if (!turns_are_time_aligned(left_turn, right_turn, policy)) continue;
          std::vector<std::string> aligned =
              lcs_tokens(left_turn.tokens, right_turn.tokens);
          const std::size_t token_total =
              left_turn.tokens.size() + right_turn.tokens.size();
          const double ratio = token_total > 0
                                   ? 2.0 * static_cast<double>(aligned.size()) /
                                         static_cast<double>(token_total)
                                   : 0.0;
          bool fingerprint_agreement = false;
          for (const AsrWord* left_word : left_turn.words) {
            DuplicateTurnMatch tentative{left_anchor.voice_group,
                                         right_anchor.voice_group,
                                         left_turn,
                                         right_turn,
                                         aligned,
                                         false};
            const AsrWord* right_word =
                aligned_word(tentative, *left_word, true, policy);
            if (right_word != nullptr && local_fingerprint_not_contrary(
                                             left, right,
                                             {left_word->start_us,
                                              left_word->end_us},
                                             {right_word->start_us,
                                              right_word->end_us},
                                             policy)) {
              fingerprint_agreement = true;
              break;
            }
          }
          const bool duplicate =
              aligned.size() >= policy.minimum_duplicate_aligned_token_count &&
              ratio > policy.minimum_duplicate_chunk_token_alignment_ratio &&
              fingerprint_agreement;
          result.chunk_content_evidence.push_back({
              left.source_ordinal,
              right.source_ordinal,
              left_turn.chunk_ordinal,
              left_turn.tokens.size(),
              right_turn.tokens.size(),
              aligned.size(),
              ratio,
              true,
              fingerprint_agreement,
              duplicate,
          });
          matches.push_back({left_anchor.voice_group,
                             right_anchor.voice_group,
                             left_turn,
                             right_turn,
                             std::move(aligned),
                             duplicate});
        }
      }
    }
  }

  std::vector<SuppressionContinuation> continuations;
  for (const DuplicateTurnMatch& match : matches) {
    if (!match.duplicate_capture_proven) continue;
    const MicrophoneTranscript& left =
        transcripts.at(transcript_by_group.at(match.left_group));
    const MicrophoneTranscript& right =
        transcripts.at(transcript_by_group.at(match.right_group));
    if (!local_fingerprint_not_contrary(left, right, match.left.timing,
                                        match.right.timing, policy)) {
      continue;
    }
    const auto left_snr = local_snr_db(left, match.left.timing);
    const auto right_snr = local_snr_db(right, match.right.timing);
    if (!left_snr.has_value() || !right_snr.has_value() ||
        *left_snr == *right_snr) {
      continue;
    }
    const bool left_is_weaker = *left_snr < *right_snr;
    continuations.push_back({
        left_is_weaker ? match.left_group : match.right_group,
        left_is_weaker ? match.right_group : match.left_group,
        match.left.chunk_ordinal,
        left_is_weaker ? match.left.timing.end_us : match.right.timing.end_us,
    });
  }

  for (const MicrophoneOwnedWordCandidate& candidate : words) {
    const auto own_found = transcript_by_group.find(candidate.voice_group);
    if (own_found == transcript_by_group.end()) continue;
    const MicrophoneTranscript& own_transcript =
        transcripts.at(own_found->second);
    bool loses_duplicate_capture = false;
    std::optional<std::size_t> stronger_source_ordinal;
    std::string suppression_reason;
    for (const DuplicateTurnMatch& match : matches) {
      if (!match.duplicate_capture_proven) continue;
      const bool own_is_left = match.left_group == candidate.voice_group;
      if (!own_is_left && match.right_group != candidate.voice_group) continue;
      const OwnershipTurn& own_turn = own_is_left ? match.left : match.right;
      if (!word_in_turn(candidate.word, own_turn)) {
        continue;
      }
      const std::size_t other_group =
          own_is_left ? match.right_group : match.left_group;
      const MicrophoneTranscript& other_transcript =
          transcripts.at(transcript_by_group.at(other_group));
      const AsrWord* other_word =
          aligned_word(match, candidate.word, own_is_left, policy);
      const OwnershipTurn& other_turn = own_is_left ? match.right : match.left;
      const TimeSpan own_timing{candidate.word.start_us,
                                candidate.word.end_us};
      const TimeSpan other_timing = other_word != nullptr
                                        ? TimeSpan{other_word->start_us,
                                                   other_word->end_us}
                                        : own_timing;
      const bool fingerprint_agreement =
          other_word != nullptr
              ? local_fingerprint_not_contrary(
                    own_transcript, other_transcript, own_timing,
                    other_timing, policy)
              : local_fingerprint_matches(
                    own_transcript, other_transcript, own_timing,
                    other_timing, policy);
      if (!fingerprint_agreement) continue;
      const std::optional<double> own_snr =
          local_snr_db(own_transcript, own_turn.timing);
      const std::optional<double> other_snr =
          local_snr_db(other_transcript, other_turn.timing);
      if (own_snr.has_value() && other_snr.has_value() &&
          *other_snr > *own_snr) {
        loses_duplicate_capture = true;
        stronger_source_ordinal = other_transcript.source_ordinal;
        suppression_reason = other_word != nullptr
                                 ? "aligned_turn_duplicate"
                                 : "fingerprinted_turn_residue";
        break;
      }
    }
    if (!loses_duplicate_capture) {
      const std::int64_t center_us =
          candidate.word.start_us +
          (candidate.word.end_us - candidate.word.start_us) / 2;
      const TimeSpan local_window = {
          std::max<std::int64_t>(
              0, center_us - policy.maximum_residual_window_radius_us),
          center_us + policy.maximum_residual_window_radius_us,
      };
      const auto own_tokens = tokens_in_window(
          own_transcript, candidate.word.chunk_ordinal, local_window);
      for (const MicrophoneSpeakerAnchor& other_anchor : anchors) {
        if (other_anchor.voice_group == candidate.voice_group) continue;
        const MicrophoneTranscript& other_transcript =
            transcripts.at(other_anchor.transcript_index);
        const auto other_tokens = tokens_in_window(
            other_transcript, candidate.word.chunk_ordinal, local_window);
        const auto aligned = lcs_tokens(own_tokens, other_tokens);
        const std::size_t token_total = own_tokens.size() + other_tokens.size();
        const double alignment_ratio = token_total > 0
            ? 2.0 * static_cast<double>(aligned.size()) /
                  static_cast<double>(token_total)
            : 0.0;
        const TimeSpan word_timing{candidate.word.start_us,
                                   candidate.word.end_us};
        if (aligned.size() < policy.minimum_duplicate_aligned_token_count ||
            alignment_ratio <=
                policy.minimum_duplicate_chunk_token_alignment_ratio ||
            !local_fingerprint_not_contrary(own_transcript, other_transcript,
                                            word_timing, word_timing, policy)) {
          continue;
        }
        const auto own_snr = local_snr_db(own_transcript, local_window);
        const auto other_snr = local_snr_db(other_transcript, local_window);
        if (own_snr.has_value() && other_snr.has_value() &&
            *other_snr > *own_snr) {
          loses_duplicate_capture = true;
          stronger_source_ordinal = other_transcript.source_ordinal;
          suppression_reason = "local_window_duplicate";
          break;
        }
      }
    }
    if (!loses_duplicate_capture) {
      const TimeSpan word_timing{candidate.word.start_us,
                                 candidate.word.end_us};
      for (SuppressionContinuation& continuation : continuations) {
        if (continuation.weaker_group != candidate.voice_group ||
            continuation.chunk_ordinal != candidate.word.chunk_ordinal ||
            candidate.word.start_us >
                continuation.current_end_us +
                    policy.maximum_speaker_segment_gap_us) {
          continue;
        }
        const bool reverse_continuation_active = std::any_of(
            continuations.begin(), continuations.end(),
            [&](const SuppressionContinuation& reverse) {
              return reverse.weaker_group == continuation.stronger_group &&
                     reverse.stronger_group == continuation.weaker_group &&
                     reverse.chunk_ordinal == continuation.chunk_ordinal &&
                     candidate.word.start_us <=
                         reverse.current_end_us +
                             policy.maximum_speaker_segment_gap_us;
            });
        if (reverse_continuation_active) continue;
        const MicrophoneTranscript& stronger = transcripts.at(
            transcript_by_group.at(continuation.stronger_group));
        const bool local_voice_available =
            dominant_track_at_timing(own_transcript, word_timing) != nullptr ||
            dominant_track_at_timing(stronger, word_timing) != nullptr;
        if (!local_voice_available &&
            mean_transcript_confidence(stronger) <=
                mean_transcript_confidence(own_transcript)) {
          continue;
        }
        const auto own_snr = local_snr_db(own_transcript, word_timing);
        const auto stronger_snr = local_snr_db(stronger, word_timing);
        if (!own_snr.has_value() || !stronger_snr.has_value() ||
            *stronger_snr <= *own_snr) {
          continue;
        }
        continuation.current_end_us =
            std::max(continuation.current_end_us, candidate.word.end_us);
        loses_duplicate_capture = true;
        stronger_source_ordinal = stronger.source_ordinal;
        suppression_reason = "duplicate_turn_continuation";
        break;
      }
    }
    if (loses_duplicate_capture) {
      result.discarded_word_evidence.push_back({
          candidate.word.text,
          candidate.word.start_us,
          candidate.word.end_us,
          candidate.word.chunk_ordinal,
          candidate.source_ordinal,
          stronger_source_ordinal,
          suppression_reason,
      });
      ++result.discarded_cross_anchor_bleed_word_count;
      ++result.discarded_word_count_by_source[candidate.source_ordinal];
      if (stronger_source_ordinal.has_value()) {
        ++result.discarded_word_count_by_source_pair[candidate.source_ordinal]
                                                   [*stronger_source_ordinal];
      }
    } else {
      result.words.push_back(candidate);
    }
  }
  return result;
}

}  // namespace svp::audio
