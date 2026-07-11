#pragma once

#include "speaker_count_estimator.hpp"

namespace svp::audio::sherpa_diarization_internal {

[[nodiscard]] bool has_fragmented_speaker_shape(
    const std::vector<SherpaDiarizationSegment>& legacy_segments,
    int32_t legacy_speaker_count,
    std::size_t legacy_observation_count);

[[nodiscard]] bool should_use_fragmented_speaker_fallback(
    const std::vector<SherpaDiarizationSegment>& legacy_segments,
    int32_t legacy_speaker_count,
    std::size_t legacy_observation_count,
    const SpeakerCountEstimate& spectral_estimate);

}  // namespace svp::audio::sherpa_diarization_internal
