#pragma once

#include "../private.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

inline constexpr std::int64_t kWordSpeakerNearestToleranceUs = 500000;
inline constexpr std::int64_t kUtteranceEmbeddingPaddingUs = 150000;
inline constexpr std::size_t kFingerprintSubUtteranceMaxWords = 4;
inline constexpr std::int64_t kFingerprintSubUtteranceMaxDurationUs = 1500000;
inline constexpr std::int64_t kFingerprintWordWindowPaddingUs = 100000;
inline constexpr float kFingerprintWordLocalMinSimilarity = 0.12f;
inline constexpr float kFingerprintWordLocalMinMargin = 0.015f;
inline constexpr std::size_t kFingerprintMaxInteriorIslandWords = 2;
inline constexpr std::size_t kFingerprintDelayedHandoffMaxWords = 3;
inline constexpr std::int64_t kFingerprintDelayedHandoffMaxDelayUs = 250000;
inline constexpr std::int64_t kFingerprintPunctuatedIslandMaxWords = 3;
inline constexpr float kFingerprintPunctuatedIslandMinSegmentOverlap = 0.50f;
inline constexpr float kSpeakerAnchorMaxSpeechSec = 4.0f;
inline constexpr float kSpeakerAnchorMinSegmentSec = 0.5f;
inline constexpr float kUtteranceEmbeddingMinSimilarity = 0.15f;
inline constexpr float kUtteranceEmbeddingMinMargin = 0.02f;
inline constexpr float kFingerprintLocalEvidenceMaxContraryMargin = 0.06f;
inline constexpr std::size_t kFingerprintLocalEvidenceMinWords = 2;
inline constexpr float kFingerprintUpdateMinMargin = 0.06f;
inline constexpr std::size_t kFingerprintUpdateMaxEmbeddings = 8;
inline constexpr float kSelectiveWordLocalUnstableGroupMargin = 0.10f;
inline constexpr std::size_t kFingerprintShortContraryMaxWords = 2;
inline constexpr float kFingerprintShortContraryMinMargin =
    2.0f * kUtteranceEmbeddingMinSimilarity;
inline constexpr std::size_t kSelectiveWordLocalBoundaryRadiusWords = 4;
inline constexpr std::int64_t kSelectiveWordLocalBoundaryRadiusUs = 1500000;
inline constexpr float kMultiSpeakerStrongSegmentOverlapLock = 0.60f;

struct VoiceFingerprint {
  std::vector<float> prototype;
  std::size_t embedding_count = 0;
};

struct SegmentOverlap {
  int32_t speaker = -1;
  float overlap_fraction = 0.0f;
};

struct AssignmentState {
  std::vector<VoiceFingerprint> fingerprints;
  std::vector<int32_t> strong_segment_assignments;
  std::vector<std::string> assignments;
  std::vector<std::vector<float>> word_embedding_similarities;
  std::vector<bool> word_local_required;
  std::vector<bool> group_decision_supported;
  bool similar_voice_fingerprints = false;
  int32_t dominant_speaker = -1;
};

std::string speaker_id_for_index(int32_t speaker_index);
int32_t speaker_index_from_id(const std::string& speaker_id,
                              int32_t speaker_count);

std::vector<float> compute_embedding_for_sample_range(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    std::int64_t start_us,
    std::int64_t end_us);

bool update_fingerprint(VoiceFingerprint& fingerprint,
                        const std::vector<float>& embedding);

std::vector<VoiceFingerprint> build_speaker_fingerprints(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const SherpaDiarizationResult& diar_result);

int32_t dominant_speaker_from_segments(
    const std::vector<SherpaDiarizationSegment>& segments,
    int32_t speaker_count);

int32_t best_segment_speaker_for_word(
    const AsrWord& word,
    const std::vector<SherpaDiarizationSegment>& segments);

SegmentOverlap best_segment_overlap_for_word(
    const AsrWord& word,
    const std::vector<SherpaDiarizationSegment>& segments);

void initialize_segment_assignments(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void assign_group_by_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state,
    std::size_t first_word,
    std::size_t last_word);

void refine_groups_by_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void mark_word_local_refinement_boundaries(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void refine_word_local_assignments(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void apply_sequence_decoder_assignments(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void repair_interior_speaker_islands(
    const std::vector<AsrWord>& words,
    AssignmentState& state);

void repair_delayed_segment_handoffs(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void repair_punctuated_segment_islands(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state);

void apply_strong_segment_locks(
    const std::vector<AsrWord>& words,
    AssignmentState& state);

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
