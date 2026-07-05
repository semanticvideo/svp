#include "private.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace svp::audio::sherpa_diarization_internal {
namespace {

constexpr std::int64_t kWordSpeakerNearestToleranceUs = 500000;
constexpr std::int64_t kUtteranceEmbeddingPaddingUs = 150000;
constexpr std::size_t kFingerprintSubUtteranceMaxWords = 4;
constexpr std::int64_t kFingerprintSubUtteranceMaxDurationUs = 1500000;
constexpr std::int64_t kFingerprintWordWindowPaddingUs = 100000;
constexpr float kFingerprintWordLocalMinSimilarity = 0.12f;
constexpr float kFingerprintWordLocalMinMargin = 0.015f;
constexpr std::size_t kFingerprintMaxInteriorIslandWords = 2;
constexpr std::size_t kFingerprintDelayedHandoffMaxWords = 3;
constexpr std::int64_t kFingerprintDelayedHandoffMaxDelayUs = 250000;
constexpr std::int64_t kFingerprintPunctuatedIslandMaxWords = 3;
constexpr float kFingerprintPunctuatedIslandMinSegmentOverlap = 0.50f;
constexpr float kSpeakerAnchorMaxSpeechSec = 4.0f;
constexpr float kSpeakerAnchorMinSegmentSec = 0.5f;
constexpr float kUtteranceEmbeddingMinSimilarity = 0.15f;
constexpr float kUtteranceEmbeddingMinMargin = 0.02f;
constexpr float kFingerprintLocalEvidenceMaxContraryMargin = 0.06f;
constexpr std::size_t kFingerprintLocalEvidenceMinWords = 2;
constexpr float kFingerprintUpdateMinMargin = 0.06f;
constexpr std::size_t kFingerprintUpdateMaxEmbeddings = 8;
constexpr float kSelectiveWordLocalUnstableGroupMargin = 0.10f;
constexpr std::size_t kSelectiveWordLocalBoundaryRadiusWords = 4;
constexpr std::int64_t kSelectiveWordLocalBoundaryRadiusUs = 1500000;
// In 3+ speaker media, clear diarization segment overlap is a stronger anchor
// than later smoothing. This prevents fingerprint/decoder passes from
// collapsing distinct speakers when the segment evidence is already decisive.
constexpr float kMultiSpeakerStrongSegmentOverlapLock = 0.60f;

struct VoiceFingerprint {
  std::vector<float> prototype;
  std::size_t embedding_count = 0;
};

std::string speaker_id_for_index(int32_t speaker_index) {
  std::ostringstream sid;
  sid << "speaker_" << std::setw(4) << std::setfill('0') << (speaker_index + 1);
  return sid.str();
}

std::size_t sample_for_us(std::int64_t timestamp_us) {
  if (timestamp_us <= 0) return 0;
  return static_cast<std::size_t>(
      (timestamp_us * kDiarizationSampleRate) / 1000000);
}

std::vector<float> compute_embedding_for_samples(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples) {
  if (!extractor || embedding_dim <= 0 ||
      samples.size() < static_cast<std::size_t>(kMinSpeakerEmbeddingSamples)) {
    return {};
  }

  const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
  if (!stream) return {};

  api.stream_accept(stream,
                    kDiarizationSampleRate,
                    samples.data(),
                    static_cast<int32_t>(samples.size()));
  api.stream_input_finished(stream);

  std::vector<float> embedding;
  if (api.emb_is_ready(extractor, stream)) {
    const float* result = api.emb_compute(extractor, stream);
    if (result) {
      embedding.assign(result, result + embedding_dim);
      api.emb_destroy_vec(result);
    }
  }
  api.stream_destroy(stream);

  if (!embedding.empty()) normalize_embedding(embedding);
  return embedding;
}

std::vector<float> compute_embedding_for_range(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const PcmS16MonoWavInfo& wav_info,
    std::int64_t start_us,
    std::int64_t end_us) {
  if (end_us <= start_us) return {};
  const std::size_t start_sample =
      std::min(sample_for_us(start_us), wav_info.sample_count);
  const std::size_t end_sample =
      std::min(sample_for_us(end_us), wav_info.sample_count);
  if (end_sample <= start_sample) return {};
  if (end_sample - start_sample <
      static_cast<std::size_t>(kMinSpeakerEmbeddingSamples)) {
    return {};
  }

  std::vector<float> samples;
  try {
    samples = read_pcm_s16le_mono_wav_range(wav_info, start_sample, end_sample);
  } catch (...) {
    return {};
  }
  return compute_embedding_for_samples(api, extractor, embedding_dim, samples);
}

std::vector<SherpaDiarizationSegment> select_speaker_anchor_segments(
    std::vector<SherpaDiarizationSegment> segments) {
  std::sort(segments.begin(), segments.end(),
            [](const SherpaDiarizationSegment& a,
               const SherpaDiarizationSegment& b) {
              return a.start_sec < b.start_sec;
            });

  std::vector<SherpaDiarizationSegment> anchors;
  float anchor_speech_sec = 0.0f;
  for (SherpaDiarizationSegment seg : segments) {
    const float duration_sec = seg.end_sec - seg.start_sec;
    if (duration_sec < kSpeakerAnchorMinSegmentSec) continue;
    if (anchor_speech_sec >= kSpeakerAnchorMaxSpeechSec) break;
    const float remaining_sec = kSpeakerAnchorMaxSpeechSec - anchor_speech_sec;
    if (duration_sec > remaining_sec) {
      seg.end_sec = seg.start_sec + remaining_sec;
    }
    anchors.push_back(seg);
    anchor_speech_sec += seg.end_sec - seg.start_sec;
  }

  if (!anchors.empty()) return anchors;
  return segments;
}

std::vector<float> compute_embedding_for_segments(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const PcmS16MonoWavInfo& wav_info,
    const std::vector<SherpaDiarizationSegment>& segments) {
  std::vector<float> samples;
  samples.reserve(static_cast<std::size_t>(kDiarizationSampleRate *
                                           kSpeakerAnchorMaxSpeechSec));

  for (const auto& seg : segments) {
    if (seg.end_sec <= seg.start_sec) continue;
    const std::size_t start_sample = std::min(
        static_cast<std::size_t>(
            std::max(0.0, std::floor(static_cast<double>(seg.start_sec) *
                                     kDiarizationSampleRate))),
        wav_info.sample_count);
    const std::size_t end_sample = std::min(
        static_cast<std::size_t>(
            std::max(0.0, std::ceil(static_cast<double>(seg.end_sec) *
                                    kDiarizationSampleRate))),
        wav_info.sample_count);
    if (end_sample <= start_sample) continue;

    std::vector<float> range;
    try {
      range = read_pcm_s16le_mono_wav_range(wav_info, start_sample, end_sample);
    } catch (...) {
      continue;
    }
    if (range.empty()) continue;

    const std::size_t remaining =
        static_cast<std::size_t>(kMaxSpeakerEmbeddingSamples) > samples.size()
            ? static_cast<std::size_t>(kMaxSpeakerEmbeddingSamples) - samples.size()
            : 0;
    if (remaining == 0) break;
    if (range.size() > remaining) {
      range.resize(remaining);
    }
    samples.insert(samples.end(), range.begin(), range.end());
  }

  return compute_embedding_for_samples(api, extractor, embedding_dim, samples);
}

bool update_fingerprint(VoiceFingerprint& fingerprint,
                        const std::vector<float>& embedding) {
  if (!has_embedding_signal(embedding)) return false;
  if (!has_embedding_signal(fingerprint.prototype)) {
    fingerprint.prototype = embedding;
    fingerprint.embedding_count = 1;
    return true;
  }
  if (fingerprint.prototype.size() != embedding.size()) return false;

  const std::size_t capped_count =
      std::min(fingerprint.embedding_count, kFingerprintUpdateMaxEmbeddings);
  const float prior_weight = static_cast<float>(capped_count);
  for (std::size_t i = 0; i < fingerprint.prototype.size(); ++i) {
    fingerprint.prototype[i] =
        (fingerprint.prototype[i] * prior_weight + embedding[i]) /
        (prior_weight + 1.0f);
  }
  normalize_embedding(fingerprint.prototype);
  ++fingerprint.embedding_count;
  return true;
}

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

struct SegmentOverlap {
  int32_t speaker = -1;
  float overlap_fraction = 0.0f;
};

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

bool has_clause_or_utterance_punctuation(const std::string& text) {
  if (text.empty()) return false;
  const char last = text.back();
  return last == '.' || last == '?' || last == '!' || last == ',' ||
         last == ';' || last == ':';
}

}  // namespace

std::vector<std::string> assign_word_speakers_with_extractor(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const PcmS16MonoWavInfo& wav_info,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result) {
  if (!extractor || embedding_dim <= 0 || words.empty() ||
      diar_result.final_speaker_count <= 1 || diar_result.segments.empty()) {
    return {};
  }

  std::vector<float> samples;
  try {
    samples = read_pcm_s16le_mono_wav_samples(wav_info.path);
  } catch (...) {
    return {};
  }
  if (samples.empty()) return {};

  auto compute_embedding_for_sample_range =
      [&](std::int64_t start_us,
          std::int64_t end_us) -> std::vector<float> {
    if (end_us <= start_us) return {};
    std::int64_t start_sample = start_us * kDiarizationSampleRate / 1000000;
    std::int64_t end_sample = end_us * kDiarizationSampleRate / 1000000;
    start_sample = std::max<std::int64_t>(0, start_sample);
    end_sample = std::min<std::int64_t>(
        static_cast<std::int64_t>(samples.size()), end_sample);
    const std::int64_t num_samples = end_sample - start_sample;
    if (num_samples < kMinSpeakerEmbeddingSamples) return {};

    const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
    if (!stream) return {};
    api.stream_accept(stream,
                      kDiarizationSampleRate,
                      samples.data() + start_sample,
                      static_cast<int32_t>(num_samples));
    api.stream_input_finished(stream);

    std::vector<float> embedding;
    if (api.emb_is_ready(extractor, stream)) {
      const float* result = api.emb_compute(extractor, stream);
      if (result) {
        embedding.assign(result, result + embedding_dim);
        api.emb_destroy_vec(result);
      }
    }
    api.stream_destroy(stream);
    if (!embedding.empty()) normalize_embedding(embedding);
    return embedding;
  };

  std::vector<std::vector<SherpaDiarizationSegment>> segments_by_speaker(
      static_cast<std::size_t>(diar_result.final_speaker_count));
  for (const auto& seg : diar_result.segments) {
    if (seg.speaker_id < 0 || seg.speaker_id >= diar_result.final_speaker_count) {
      continue;
    }
    segments_by_speaker[static_cast<std::size_t>(seg.speaker_id)].push_back(seg);
  }

  std::vector<VoiceFingerprint> fingerprints(
      static_cast<std::size_t>(diar_result.final_speaker_count));
  for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
    const std::vector<SherpaDiarizationSegment> anchors =
        select_speaker_anchor_segments(
            segments_by_speaker[static_cast<std::size_t>(speaker)]);
    std::vector<float> anchor_embedding =
        compute_bounded_speaker_embedding(
            api, extractor, embedding_dim, samples, 0, anchors);
    if (!update_fingerprint(fingerprints[static_cast<std::size_t>(speaker)],
                            anchor_embedding) &&
        static_cast<std::size_t>(speaker) <
            diar_result.final_speaker_fingerprints.size()) {
      update_fingerprint(
          fingerprints[static_cast<std::size_t>(speaker)],
          diar_result.final_speaker_fingerprints[static_cast<std::size_t>(speaker)]);
    }
  }

  for (const auto& fingerprint : fingerprints) {
    if (!has_embedding_signal(fingerprint.prototype)) return {};
  }

  const int32_t dominant_speaker =
      dominant_speaker_from_segments(diar_result.segments,
                                     diar_result.final_speaker_count);

  std::vector<int32_t> segment_assignments(words.size(), -1);
  std::vector<int32_t> strong_segment_assignments(words.size(), -1);
  std::vector<std::string> assignments(words.size(), "speaker_unknown");
  std::vector<std::vector<float>> word_embedding_similarities(
      words.size(),
      std::vector<float>(
          static_cast<std::size_t>(diar_result.final_speaker_count), -2.0f));
  std::vector<bool> word_local_required(words.size(), false);
  for (std::size_t i = 0; i < words.size(); ++i) {
    segment_assignments[i] =
        best_segment_speaker_for_word(words[i], diar_result.segments);
    const SegmentOverlap overlap =
        best_segment_overlap_for_word(words[i], diar_result.segments);
    if (diar_result.final_speaker_count > 2 &&
        overlap.speaker >= 0 &&
        overlap.overlap_fraction >= kMultiSpeakerStrongSegmentOverlapLock) {
      strong_segment_assignments[i] = overlap.speaker;
    }
    if (segment_assignments[i] >= 0) {
      assignments[i] = speaker_id_for_index(segment_assignments[i]);
    } else {
      word_local_required[i] = true;
    }
  }

  auto current_speaker_index = [&](const std::string& speaker_id) -> int32_t {
    if (speaker_id.rfind("speaker_", 0) != 0) return -1;
    try {
      const int32_t one_based = std::stoi(speaker_id.substr(8));
      const int32_t zero_based = one_based - 1;
      if (zero_based >= 0 && zero_based < diar_result.final_speaker_count) {
        return zero_based;
      }
    } catch (...) {
      return -1;
    }
    return -1;
  };

  auto assign_group_by_embedding = [&](std::size_t first_word,
                                       std::size_t last_word) {
    if (first_word > last_word || last_word >= words.size()) return;
    const std::int64_t start_us =
        std::max<std::int64_t>(0, words[first_word].start_us -
                                      kUtteranceEmbeddingPaddingUs);
    const std::int64_t end_us =
        words[last_word].end_us + kUtteranceEmbeddingPaddingUs;
    std::vector<float> group_embedding =
        compute_embedding_for_sample_range(start_us, end_us);
    if (!has_embedding_signal(group_embedding)) return;

    int32_t best_speaker = -1;
    float best_similarity = -2.0f;
    float second_similarity = -2.0f;
    std::vector<float> similarities(
        static_cast<std::size_t>(diar_result.final_speaker_count), -2.0f);
    for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
      const float similarity = cosine_similarity(
          group_embedding,
          fingerprints[static_cast<std::size_t>(speaker)].prototype);
      similarities[static_cast<std::size_t>(speaker)] = similarity;
      if (similarity > best_similarity) {
        second_similarity = best_similarity;
        best_similarity = similarity;
        best_speaker = speaker;
      } else if (similarity > second_similarity) {
        second_similarity = similarity;
      }
    }

    std::vector<std::size_t> local_counts(
        static_cast<std::size_t>(diar_result.final_speaker_count), 0);
    for (std::size_t i = first_word; i <= last_word; ++i) {
      const int32_t speaker = current_speaker_index(assignments[i]);
      if (speaker >= 0) ++local_counts[static_cast<std::size_t>(speaker)];
    }

    int32_t local_evidence_speaker = -1;
    std::size_t local_evidence_count = 0;
    for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
      if (speaker == dominant_speaker) continue;
      const std::size_t count = local_counts[static_cast<std::size_t>(speaker)];
      if (count > local_evidence_count) {
        local_evidence_speaker = speaker;
        local_evidence_count = count;
      }
    }

    const float margin = best_similarity - second_similarity;
    int32_t selected_speaker = -1;
    if (local_evidence_speaker >= 0 &&
        local_evidence_count >= kFingerprintLocalEvidenceMinWords &&
        similarities[static_cast<std::size_t>(local_evidence_speaker)] >=
            kUtteranceEmbeddingMinSimilarity &&
        best_similarity -
            similarities[static_cast<std::size_t>(local_evidence_speaker)] <=
            kFingerprintLocalEvidenceMaxContraryMargin) {
      selected_speaker = local_evidence_speaker;
    } else if (best_speaker >= 0 &&
               best_similarity >= kUtteranceEmbeddingMinSimilarity &&
               margin >= kUtteranceEmbeddingMinMargin) {
      selected_speaker = best_speaker;
    }

    svp::core::trace_memory_event("diarization.asr_word_fingerprint.group", {
        {"first_word", std::to_string(first_word)},
        {"last_word", std::to_string(last_word)},
        {"best_speaker", std::to_string(best_speaker)},
        {"selected_speaker", std::to_string(selected_speaker)},
        {"best_similarity", std::to_string(best_similarity)},
        {"second_similarity", std::to_string(second_similarity)},
        {"margin", std::to_string(margin)},
        {"local_evidence_speaker", std::to_string(local_evidence_speaker)},
        {"local_evidence_count", std::to_string(local_evidence_count)}
    });

    if (selected_speaker < 0) return;

    const std::string speaker_id = speaker_id_for_index(selected_speaker);
    for (std::size_t i = first_word; i <= last_word; ++i) {
      assignments[i] = speaker_id;
    }
    if (margin < kSelectiveWordLocalUnstableGroupMargin) {
      for (std::size_t i = first_word; i <= last_word; ++i) {
        word_local_required[i] = true;
      }
    }

    if (margin >= kFingerprintUpdateMinMargin) {
      update_fingerprint(
          fingerprints[static_cast<std::size_t>(selected_speaker)],
          group_embedding);
    }
  };

  auto group_has_non_dominant_local_evidence = [&](std::size_t first_word,
                                                   std::size_t last_word) {
    for (std::size_t i = first_word; i <= last_word && i < assignments.size(); ++i) {
      const int32_t speaker = current_speaker_index(assignments[i]);
      if (speaker >= 0 && speaker != dominant_speaker) return true;
    }
    return false;
  };

  auto assign_group_or_subgroups = [&](std::size_t first_word,
                                       std::size_t last_word) {
    if (first_word > last_word) return;
    const bool has_local_evidence =
        group_has_non_dominant_local_evidence(first_word, last_word);
    const std::int64_t duration_us =
        words[last_word].end_us - words[first_word].start_us;
    const std::size_t word_count = last_word - first_word + 1;
    if (has_local_evidence ||
        (word_count <= kFingerprintSubUtteranceMaxWords &&
         duration_us <= kFingerprintSubUtteranceMaxDurationUs)) {
      assign_group_by_embedding(first_word, last_word);
      return;
    }

    if (diar_result.final_speaker_count > 2) {
      assign_group_by_embedding(first_word, last_word);
      return;
    }

    std::size_t sub_start = first_word;
    while (sub_start <= last_word) {
      std::size_t sub_end = sub_start;
      while (sub_end < last_word &&
             sub_end - sub_start + 1 < kFingerprintSubUtteranceMaxWords &&
             words[sub_end + 1].end_us - words[sub_start].start_us <=
                 kFingerprintSubUtteranceMaxDurationUs) {
        ++sub_end;
      }
      assign_group_by_embedding(sub_start, sub_end);
      sub_start = sub_end + 1;
    }
  };

  std::size_t group_start = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const bool last_word = i + 1 == words.size();
    const bool gap_after =
        !last_word &&
        words[i + 1].start_us - words[i].end_us > kUtteranceGapThresholdUs;
    if (last_word || gap_after || ends_utterance(words[i].text)) {
      assign_group_or_subgroups(group_start, i);
      group_start = i + 1;
    }
  }

  auto mark_word_local_boundary = [&](std::int64_t boundary_us) {
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
        word_local_required[j] = true;
      }
    }
  };

  for (std::size_t i = 1; i < words.size(); ++i) {
    const int32_t previous_speaker = current_speaker_index(assignments[i - 1]);
    const int32_t speaker = current_speaker_index(assignments[i]);
    if (previous_speaker >= 0 && speaker >= 0 && previous_speaker != speaker) {
      mark_word_local_boundary(words[i].start_us);
    }
  }
  if (diar_result.final_speaker_count == 2) {
    for (std::size_t seg_index = 1; seg_index < diar_result.segments.size();
         ++seg_index) {
      const auto& previous = diar_result.segments[seg_index - 1];
      const auto& segment = diar_result.segments[seg_index];
      if (previous.speaker_id != segment.speaker_id) {
        mark_word_local_boundary(
            static_cast<std::int64_t>(segment.start_sec * 1000000.0f));
      }
    }
  }
  std::size_t word_local_embedding_requests = 0;
  for (std::size_t word_index = 0; word_index < words.size(); ++word_index) {
    if (!word_local_required[word_index]) continue;
    const std::int64_t start_us =
        std::max<std::int64_t>(0, words[word_index].start_us -
                                      kFingerprintWordWindowPaddingUs);
    const std::int64_t end_us =
        words[word_index].end_us + kFingerprintWordWindowPaddingUs;
    std::vector<float> word_embedding =
        compute_embedding_for_sample_range(start_us, end_us);
    if (!has_embedding_signal(word_embedding)) continue;

    int32_t best_speaker = -1;
    float best_similarity = -2.0f;
    float second_similarity = -2.0f;
    std::vector<float> similarities(
        static_cast<std::size_t>(diar_result.final_speaker_count), -2.0f);
    for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
      const float similarity = cosine_similarity(
          word_embedding,
          fingerprints[static_cast<std::size_t>(speaker)].prototype);
      similarities[static_cast<std::size_t>(speaker)] = similarity;
      if (similarity > best_similarity) {
        second_similarity = best_similarity;
        best_similarity = similarity;
        best_speaker = speaker;
      } else if (similarity > second_similarity) {
        second_similarity = similarity;
      }
    }

    if (best_speaker < 0 ||
        best_similarity < kFingerprintWordLocalMinSimilarity ||
        best_similarity - second_similarity < kFingerprintWordLocalMinMargin) {
      continue;
    }
    assignments[word_index] = speaker_id_for_index(best_speaker);
    word_embedding_similarities[word_index] = similarities;
    ++word_local_embedding_requests;
  }
  svp::core::trace_memory_event("diarization.asr_word_fingerprint.selective_word_local", {
      {"word_count", std::to_string(words.size())},
      {"word_local_embeddings", std::to_string(word_local_embedding_requests)}
  });

  std::vector<WordSpeakerEvidence> decoder_evidence(words.size());
  for (std::size_t i = 0; i < words.size(); ++i) {
    decoder_evidence[i].segment_speaker = current_speaker_index(assignments[i]);
    decoder_evidence[i].embedding_similarity_by_speaker =
        word_embedding_similarities[i];
  }

  const std::vector<int32_t> decoded =
      decode_word_speaker_sequence(
          words, diar_result.final_speaker_count, decoder_evidence);
  if (decoded.size() == assignments.size()) {
    for (std::size_t i = 0; i < assignments.size(); ++i) {
      if (decoded[i] >= 0) {
        assignments[i] = speaker_id_for_index(decoded[i]);
      }
    }
  }

  std::size_t run_start = 0;
  while (run_start < assignments.size()) {
    std::size_t run_end = run_start;
    while (run_end + 1 < assignments.size() &&
           assignments[run_end + 1] == assignments[run_start]) {
      ++run_end;
    }

    const std::size_t run_words = run_end - run_start + 1;
    const bool fluent_after =
        run_end + 1 < words.size() &&
        words[run_end + 1].start_us - words[run_end].end_us <=
            kUtteranceGapThresholdUs;
    if (run_start > 0 &&
        run_end + 1 < assignments.size() &&
        run_words <= kFingerprintMaxInteriorIslandWords &&
        fluent_after &&
        assignments[run_start - 1] == assignments[run_end + 1] &&
        assignments[run_start] != assignments[run_start - 1]) {
      for (std::size_t i = run_start; i <= run_end; ++i) {
        assignments[i] = assignments[run_start - 1];
      }
    }

    run_start = run_end + 1;
  }

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
        assignments[first_word] == segment_speaker_id) {
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
      if (assignments[i] == segment_speaker_id) {
        natural_switch = i;
        break;
      }
      previous_end_us = words[i].end_us;
    }
    if (natural_switch == words.size()) continue;

    for (std::size_t i = first_word; i < natural_switch; ++i) {
      assignments[i] = segment_speaker_id;
    }
  }

  std::size_t punct_run_start = 0;
  while (punct_run_start < assignments.size()) {
    std::size_t punct_run_end = punct_run_start;
    while (punct_run_end + 1 < assignments.size() &&
           assignments[punct_run_end + 1] == assignments[punct_run_start]) {
      ++punct_run_end;
    }

    const std::size_t run_words = punct_run_end - punct_run_start + 1;
    if (run_words <= kFingerprintPunctuatedIslandMaxWords) {
      bool has_punctuation = false;
      for (std::size_t i = punct_run_start; i <= punct_run_end; ++i) {
        if (has_clause_or_utterance_punctuation(words[i].text)) {
          has_punctuation = true;
          break;
        }
      }

      if (has_punctuation) {
        int32_t segment_speaker = -1;
        bool all_words_match_segment = true;
        for (std::size_t i = punct_run_start; i <= punct_run_end; ++i) {
          const SegmentOverlap overlap =
              best_segment_overlap_for_word(words[i], diar_result.segments);
          if (overlap.speaker < 0 ||
              overlap.overlap_fraction <
                  kFingerprintPunctuatedIslandMinSegmentOverlap ||
              current_speaker_index(assignments[i]) == overlap.speaker) {
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
          for (std::size_t i = punct_run_start; i <= punct_run_end; ++i) {
            assignments[i] = segment_speaker_id;
          }
        }
      }
    }

    punct_run_start = punct_run_end + 1;
  }

  std::size_t strong_segment_locks_applied = 0;
  for (std::size_t i = 0; i < assignments.size(); ++i) {
    if (strong_segment_assignments[i] < 0) continue;
    const std::string segment_speaker_id =
        speaker_id_for_index(strong_segment_assignments[i]);
    if (assignments[i] != segment_speaker_id) {
      assignments[i] = segment_speaker_id;
      ++strong_segment_locks_applied;
    }
  }
  if (strong_segment_locks_applied > 0) {
    svp::core::trace_memory_event("diarization.asr_word_fingerprint.segment_lock", {
        {"word_count", std::to_string(words.size())},
        {"locks_applied", std::to_string(strong_segment_locks_applied)}
    });
  }

  return assignments;
}

}  // namespace svp::audio::sherpa_diarization_internal
