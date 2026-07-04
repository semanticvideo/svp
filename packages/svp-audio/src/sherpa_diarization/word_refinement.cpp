#include "private.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace svp::audio::sherpa_diarization_internal {

bool ends_utterance(const std::string& text) {
  if (text.empty()) return false;
  const char last = text.back();
  return last == '.' || last == '?' || last == '!';
}

}  // namespace svp::audio::sherpa_diarization_internal

namespace svp::audio {

using namespace sherpa_diarization_internal;

namespace {

constexpr float kUtteranceEmbeddingMinSimilarity = 0.15f;
constexpr float kUtteranceEmbeddingMinMargin = 0.02f;
constexpr float kFingerprintAmbiguousMargin = 0.04f;
constexpr float kFingerprintLocalEvidenceMaxContraryMargin = 0.06f;
constexpr float kFingerprintUpdateMinMargin = 0.06f;
constexpr std::int64_t kWordSpeakerNearestToleranceUs = 500000;
constexpr std::int64_t kUtteranceEmbeddingPaddingUs = 150000;
constexpr float kSpeakerAnchorMaxSpeechSec = 4.0f;
constexpr float kSpeakerAnchorMinSegmentSec = 0.5f;
constexpr std::size_t kFingerprintLocalEvidenceMinWords = 2;
constexpr std::size_t kFingerprintUpdateMaxEmbeddings = 8;
constexpr std::size_t kFingerprintSubUtteranceMaxWords = 4;
constexpr std::int64_t kFingerprintSubUtteranceMaxDurationUs = 1500000;
constexpr std::int64_t kFingerprintWordWindowPaddingUs = 100000;
constexpr float kFingerprintWordLocalMinSimilarity = 0.12f;
constexpr float kFingerprintWordLocalMinMargin = 0.015f;
constexpr std::size_t kFingerprintMaxInteriorIslandWords = 2;

std::string speaker_id_for_index(int32_t speaker_index) {
  std::ostringstream sid;
  sid << "speaker_" << std::setw(4) << std::setfill('0') << (speaker_index + 1);
  return sid.str();
}

std::string segment_overlap_assignment(
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
  if (best_seg) {
    return speaker_id_for_index(best_seg->speaker_id);
  }

  const SherpaDiarizationSegment* nearest_seg = nullptr;
  std::int64_t nearest_dist = std::numeric_limits<std::int64_t>::max();
  for (const auto& seg : segments) {
    const std::int64_t seg_start =
        static_cast<std::int64_t>(seg.start_sec * 1000000.0f);
    const std::int64_t seg_end =
        static_cast<std::int64_t>(seg.end_sec * 1000000.0f);
    std::int64_t dist;
    if (word.end_us <= seg_start) {
      dist = seg_start - word.end_us;
    } else if (word.start_us >= seg_end) {
      dist = word.start_us - seg_end;
    } else {
      dist = 0;
    }
    if (dist < nearest_dist) {
      nearest_dist = dist;
      nearest_seg = &seg;
    }
  }
  if (nearest_seg && nearest_dist <= kWordSpeakerNearestToleranceUs) {
    return speaker_id_for_index(nearest_seg->speaker_id);
  }
  return "speaker_unknown";
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

struct VoiceFingerprint {
  std::vector<float> prototype;
  std::size_t embedding_count = 0;
};

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

}  // namespace

std::vector<std::string> refine_word_speakers_by_embedding(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result) {

  if (words.empty() || diar_result.final_speaker_count <= 1 ||
      diar_result.segments.empty()) {
    return {};
  }

  const SherpaDiarizationApi& api = get_api();
  if (!api.lib_handle || !api.emb_create) {
    return {};
  }

  const std::filesystem::path embedding_model =
      model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
  if (!std::filesystem::exists(embedding_model)) {
    return {};
  }

  std::vector<float> samples;
  try {
    samples = read_pcm_s16le_mono_wav_samples(wav_path);
  } catch (...) {
    return {};
  }
  if (samples.empty()) return {};

  const std::string emb_path = embedding_model.string();
  SherpaOnnxSpeakerEmbeddingExtractorConfig emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  const void* extractor = api.emb_create(&emb_config);
  if (!extractor) return {};

  const int32_t embedding_dim = api.emb_dim(extractor);

  auto compute_embedding_for_range = [&](std::int64_t start_us,
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
    const std::vector<SherpaDiarizationSegment> anchor_segments =
        select_speaker_anchor_segments(
            segments_by_speaker[static_cast<std::size_t>(speaker)]);
    std::vector<float> anchor_embedding =
        compute_bounded_speaker_embedding(
            api,
            extractor,
            embedding_dim,
            samples,
            0,
            anchor_segments);
    if (!update_fingerprint(fingerprints[static_cast<std::size_t>(speaker)],
                            anchor_embedding)) {
      api.emb_destroy(extractor);
      return {};
    }
  }

  const int32_t dominant_speaker =
      dominant_speaker_from_segments(diar_result.segments,
                                     diar_result.final_speaker_count);

  std::vector<std::string> assignments(words.size(), "speaker_unknown");
  for (std::size_t i = 0; i < words.size(); ++i) {
    assignments[i] = segment_overlap_assignment(words[i], diar_result.segments);
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
        compute_embedding_for_range(start_us, end_us);
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

    svp::core::trace_memory_event("diarization.voice_fingerprint.utterance", {
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

    if (selected_speaker < 0) {
      return;
    }

    const std::string speaker_id = speaker_id_for_index(selected_speaker);
    for (std::size_t i = first_word; i <= last_word; ++i) {
      assignments[i] = speaker_id;
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

  for (std::size_t word_index = 0; word_index < words.size(); ++word_index) {
    const std::int64_t start_us =
        std::max<std::int64_t>(0, words[word_index].start_us -
                                      kFingerprintWordWindowPaddingUs);
    const std::int64_t end_us =
        words[word_index].end_us + kFingerprintWordWindowPaddingUs;
    std::vector<float> word_embedding =
        compute_embedding_for_range(start_us, end_us);
    if (!has_embedding_signal(word_embedding)) continue;

    int32_t best_speaker = -1;
    float best_similarity = -2.0f;
    float second_similarity = -2.0f;
    for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
      const float similarity = cosine_similarity(
          word_embedding,
          fingerprints[static_cast<std::size_t>(speaker)].prototype);
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
  }

  std::size_t run_start = 0;
  while (run_start < assignments.size()) {
    std::size_t run_end = run_start;
    while (run_end + 1 < assignments.size() &&
           assignments[run_end + 1] == assignments[run_start]) {
      ++run_end;
    }

    const std::size_t run_words = run_end - run_start + 1;
    if (run_start > 0 &&
        run_end + 1 < assignments.size() &&
        run_words <= kFingerprintMaxInteriorIslandWords &&
        !ends_utterance(words[run_end].text) &&
        assignments[run_start - 1] == assignments[run_end + 1] &&
        assignments[run_start] != assignments[run_start - 1]) {
      for (std::size_t i = run_start; i <= run_end; ++i) {
        assignments[i] = assignments[run_start - 1];
      }
    }

    run_start = run_end + 1;
  }

  api.emb_destroy(extractor);
  return assignments;
}

}  // namespace svp::audio
