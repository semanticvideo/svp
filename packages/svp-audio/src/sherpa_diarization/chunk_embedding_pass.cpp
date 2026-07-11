#include "chunk_embedding_pass.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace svp::audio::sherpa_diarization_internal {

void populate_chunk_speaker_embeddings(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const PcmS16MonoWavInfo& wav_info,
    std::vector<SpeakerObservation>& observations,
    const std::vector<SherpaDiarizationSegment>& segments) {
  if (!extractor || embedding_dim <= 0) return;
  std::map<int32_t, std::vector<SherpaDiarizationSegment>> by_observation;
  for (const auto& segment : segments) {
    by_observation[segment.speaker_id].push_back(segment);
  }
  for (auto& observation : observations) {
    const auto found = by_observation.find(observation.observation_id);
    if (found == by_observation.end() || found->second.empty()) continue;
    const auto& observation_segments = found->second;
    const float first_start = std::min_element(
        observation_segments.begin(), observation_segments.end(),
        [](const auto& left, const auto& right) {
          return left.start_sec < right.start_sec;
        })->start_sec;
    const float last_end = std::max_element(
        observation_segments.begin(), observation_segments.end(),
        [](const auto& left, const auto& right) {
          return left.end_sec < right.end_sec;
        })->end_sec;
    const std::size_t start_sample = static_cast<std::size_t>(
        std::max(0.0f, std::floor(first_start * kDiarizationSampleRate)));
    const std::size_t end_sample = std::min(
        wav_info.sample_count,
        static_cast<std::size_t>(
            std::ceil(last_end * kDiarizationSampleRate)));
    if (end_sample <= start_sample) continue;
    const std::vector<float> samples = read_pcm_s16le_mono_wav_range(
        wav_info, start_sample, end_sample);
    observation.embedding = compute_bounded_speaker_embedding(
        api, extractor, embedding_dim, samples, start_sample,
        observation_segments);
  }
}

}  // namespace svp::audio::sherpa_diarization_internal
