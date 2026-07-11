#pragma once

#include "private.hpp"

namespace svp::audio::sherpa_diarization_internal {

void populate_chunk_speaker_embeddings(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const PcmS16MonoWavInfo& wav_info,
    std::vector<SpeakerObservation>& observations,
    const std::vector<SherpaDiarizationSegment>& segments);

}  // namespace svp::audio::sherpa_diarization_internal
