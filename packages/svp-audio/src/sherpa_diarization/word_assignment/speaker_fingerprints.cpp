#include "types.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {
namespace {

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

}  // namespace

std::vector<VoiceFingerprint> build_speaker_fingerprints(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const SherpaDiarizationResult& diar_result) {
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
  return fingerprints;
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
