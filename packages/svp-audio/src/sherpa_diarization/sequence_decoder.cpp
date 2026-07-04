#include "private.hpp"

#include <algorithm>
#include <limits>

namespace svp::audio::sherpa_diarization_internal {
namespace {

// The decoder combines two independent local signals:
// 1. segment-overlap speaker evidence from diarization timing;
// 2. embedding similarity evidence from short local audio windows.
//
// Switch penalties are intentionally continuity-biased inside a fluent phrase,
// then relaxed at gaps, punctuation, or diarization segment boundaries where a
// real turn change is more likely.
constexpr float kDecoderSegmentAgreementScore = 1.0f;
constexpr float kDecoderSegmentDisagreementPenalty = 0.35f;
constexpr float kDecoderEmbeddingScoreWeight = 4.0f;
constexpr float kDecoderUnknownSpeakerPenalty = 0.20f;
constexpr float kDecoderFluentSwitchPenalty = 0.85f;
constexpr float kDecoderBoundarySwitchPenalty = 0.15f;
constexpr float kDecoderSegmentBoundarySwitchPenalty = 0.25f;
constexpr float kDecoderMinScore = -1.0e20f;

bool valid_speaker(int32_t speaker, int32_t speaker_count) {
  return speaker >= 0 && speaker < speaker_count;
}

float embedding_emission(const WordSpeakerEvidence& evidence, int32_t speaker) {
  if (speaker < 0 ||
      static_cast<std::size_t>(speaker) >=
          evidence.embedding_similarity_by_speaker.size()) {
    return 0.0f;
  }

  const float similarity =
      evidence.embedding_similarity_by_speaker[static_cast<std::size_t>(speaker)];
  if (similarity <= -1.0f) return 0.0f;
  return similarity * kDecoderEmbeddingScoreWeight;
}

float segment_emission(const WordSpeakerEvidence& evidence,
                       int32_t speaker,
                       int32_t speaker_count) {
  if (!valid_speaker(evidence.segment_speaker, speaker_count)) {
    return -kDecoderUnknownSpeakerPenalty;
  }
  if (evidence.segment_speaker == speaker) {
    return kDecoderSegmentAgreementScore;
  }
  return -kDecoderSegmentDisagreementPenalty;
}

float emission_score(const WordSpeakerEvidence& evidence,
                     int32_t speaker,
                     int32_t speaker_count) {
  return segment_emission(evidence, speaker, speaker_count) +
         embedding_emission(evidence, speaker);
}

bool has_boundary_after(const std::vector<AsrWord>& words, std::size_t index) {
  if (index + 1 >= words.size()) return true;
  if (ends_utterance(words[index].text)) return true;
  return words[index + 1].start_us - words[index].end_us >
         kUtteranceGapThresholdUs;
}

float switch_penalty(const std::vector<AsrWord>& words,
                     const std::vector<WordSpeakerEvidence>& evidence,
                     std::size_t previous_index) {
  if (has_boundary_after(words, previous_index)) {
    return kDecoderBoundarySwitchPenalty;
  }
  if (previous_index + 1 < evidence.size() &&
      evidence[previous_index].segment_speaker >= 0 &&
      evidence[previous_index + 1].segment_speaker >= 0 &&
      evidence[previous_index].segment_speaker !=
          evidence[previous_index + 1].segment_speaker) {
    return kDecoderSegmentBoundarySwitchPenalty;
  }
  return kDecoderFluentSwitchPenalty;
}

}  // namespace

std::vector<int32_t> decode_word_speaker_sequence(
    const std::vector<AsrWord>& words,
    int32_t speaker_count,
    const std::vector<WordSpeakerEvidence>& evidence) {
  if (words.empty() || speaker_count <= 0 || evidence.size() != words.size()) {
    return {};
  }

  const std::size_t word_count = words.size();
  const std::size_t speakers = static_cast<std::size_t>(speaker_count);
  std::vector<std::vector<float>> score(
      word_count, std::vector<float>(speakers, kDecoderMinScore));
  std::vector<std::vector<int32_t>> backptr(
      word_count, std::vector<int32_t>(speakers, -1));

  for (int32_t speaker = 0; speaker < speaker_count; ++speaker) {
    score[0][static_cast<std::size_t>(speaker)] =
        emission_score(evidence[0], speaker, speaker_count);
  }

  for (std::size_t i = 1; i < word_count; ++i) {
    const float change_penalty = switch_penalty(words, evidence, i - 1);
    for (int32_t speaker = 0; speaker < speaker_count; ++speaker) {
      const float emit = emission_score(evidence[i], speaker, speaker_count);
      float best_score = kDecoderMinScore;
      int32_t best_previous = -1;
      for (int32_t previous = 0; previous < speaker_count; ++previous) {
        float candidate = score[i - 1][static_cast<std::size_t>(previous)];
        if (previous != speaker) candidate -= change_penalty;
        if (candidate > best_score) {
          best_score = candidate;
          best_previous = previous;
        }
      }
      score[i][static_cast<std::size_t>(speaker)] = best_score + emit;
      backptr[i][static_cast<std::size_t>(speaker)] = best_previous;
    }
  }

  int32_t best_final = 0;
  float best_final_score = kDecoderMinScore;
  for (int32_t speaker = 0; speaker < speaker_count; ++speaker) {
    const float candidate =
        score[word_count - 1][static_cast<std::size_t>(speaker)];
    if (candidate > best_final_score) {
      best_final_score = candidate;
      best_final = speaker;
    }
  }

  std::vector<int32_t> decoded(word_count, -1);
  int32_t current = best_final;
  for (std::size_t reverse = word_count; reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    decoded[index] = current;
    current = backptr[index][static_cast<std::size_t>(current)];
    if (current < 0 && index > 0) current = decoded[index];
  }

  return decoded;
}

}  // namespace svp::audio::sherpa_diarization_internal
