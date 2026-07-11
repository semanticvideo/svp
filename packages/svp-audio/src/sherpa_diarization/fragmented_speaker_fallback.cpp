#include "fragmented_speaker_fallback.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace svp::audio::sherpa_diarization_internal {
namespace {

// Spectral fallback resolves shallow gap-cut fragmentation. Dense observation
// histories represent sustained speaker diversity and stay on legacy policy.
constexpr std::size_t kMaximumObservationsPerLegacySpeaker = 2;

}  // namespace

bool has_fragmented_speaker_shape(
    const std::vector<SherpaDiarizationSegment>& legacy_segments,
    int32_t legacy_speaker_count,
    std::size_t legacy_observation_count) {
  if (legacy_speaker_count < kFragmentedSecondaryMinFinalSpeakers ||
      legacy_observation_count < kFragmentedSecondaryMinObservations ||
      legacy_observation_count >
          static_cast<std::size_t>(legacy_speaker_count) *
              kMaximumObservationsPerLegacySpeaker) {
    return false;
  }

  std::vector<std::int64_t> speech_us(
      static_cast<std::size_t>(legacy_speaker_count), 0);
  std::int64_t total_speech_us = 0;
  for (const auto& segment : legacy_segments) {
    if (segment.speaker_id < 0 ||
        segment.speaker_id >= legacy_speaker_count) {
      continue;
    }
    const std::int64_t duration_us = static_cast<std::int64_t>(
        std::max(0.0f, segment.end_sec - segment.start_sec) * 1000000.0f);
    speech_us[static_cast<std::size_t>(segment.speaker_id)] += duration_us;
    total_speech_us += duration_us;
  }
  if (total_speech_us <= 0) return false;

  const auto dominant = std::max_element(speech_us.begin(), speech_us.end());
  const std::int64_t dominant_speech_us = *dominant;
  std::int64_t largest_minority_speech_us = 0;
  for (auto speaker = speech_us.begin(); speaker != speech_us.end(); ++speaker) {
    if (speaker == dominant) continue;
    largest_minority_speech_us =
        std::max(largest_minority_speech_us, *speaker);
  }
  const std::int64_t minority_speech_us =
      total_speech_us - dominant_speech_us;
  const float dominant_share = static_cast<float>(dominant_speech_us) /
                               static_cast<float>(total_speech_us);
  const float minority_share = static_cast<float>(minority_speech_us) /
                               static_cast<float>(total_speech_us);
  const float largest_minority_share =
      static_cast<float>(largest_minority_speech_us) /
      static_cast<float>(total_speech_us);
  return dominant_share >= kFragmentedSecondaryDominantMinShare &&
         dominant_share <= kFragmentedSecondaryDominantMaxShare &&
         minority_share >= kFragmentedSecondaryMinMinorityShare &&
         largest_minority_share <=
             kFragmentedSecondaryMaxSingleMinorityShare;
}

bool should_use_fragmented_speaker_fallback(
    const std::vector<SherpaDiarizationSegment>& legacy_segments,
    int32_t legacy_speaker_count,
    std::size_t legacy_observation_count,
    const SpeakerCountEstimate& spectral_estimate) {
  return has_fragmented_speaker_shape(
             legacy_segments, legacy_speaker_count,
             legacy_observation_count) &&
         spectral_estimate.speaker_count >= 2 &&
         spectral_estimate.speaker_count <
             static_cast<std::size_t>(legacy_speaker_count) &&
         spectral_estimate.assignments_respect_cannot_link &&
         !spectral_estimate.observation_to_speaker.empty();
}

}  // namespace svp::audio::sherpa_diarization_internal
