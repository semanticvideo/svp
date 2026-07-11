#include "private.hpp"
#include "word_assignment/types.hpp"

namespace svp::audio::sherpa_diarization_internal {
using namespace word_assignment;

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

  AssignmentState state;
  state.fingerprints =
      build_speaker_fingerprints(api, extractor, embedding_dim, samples, diar_result);
  for (const auto& fingerprint : state.fingerprints) {
    if (!has_embedding_signal(fingerprint.prototype)) return {};
  }
  if (state.fingerprints.size() == 2) {
    const float fingerprint_similarity = cosine_similarity(
        state.fingerprints[0].prototype,
        state.fingerprints[1].prototype);
    state.similar_voice_fingerprints =
        fingerprint_similarity >= kUtteranceEmbeddingMinSimilarity;
  }
  state.dominant_speaker =
      dominant_speaker_from_segments(diar_result.segments,
                                     diar_result.final_speaker_count);

  initialize_segment_assignments(words, diar_result, state);
  refine_groups_by_embedding(
      api, extractor, embedding_dim, samples, words, diar_result, state);
  refine_word_local_assignments(
      api, extractor, embedding_dim, samples, words, diar_result, state);
  apply_sequence_decoder_assignments(words, diar_result, state);
  repair_interior_speaker_islands(words, state);
  repair_delayed_segment_handoffs(words, diar_result, state);
  repair_punctuated_segment_islands(words, diar_result, state);
  apply_strong_segment_locks(words, state);

  return state.assignments;
}

}  // namespace svp::audio::sherpa_diarization_internal
