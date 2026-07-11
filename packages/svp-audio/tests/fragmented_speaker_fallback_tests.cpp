#include "audio_test_support.hpp"
#include "../src/sherpa_diarization/fragmented_speaker_fallback.hpp"

void test_fragmented_speaker_fallback_accepts_dominant_fragment_shape() {
  using namespace svp::audio::sherpa_diarization_internal;
  std::vector<svp::audio::SherpaDiarizationSegment> segments = {
      {0.0f, 80.0f, 0},
      {80.0f, 84.0f, 1},
      {84.0f, 88.0f, 2},
      {88.0f, 92.0f, 3},
      {92.0f, 96.0f, 4},
      {96.0f, 100.0f, 5},
  };
  SpeakerCountEstimate estimate;
  estimate.speaker_count = 2;
  estimate.observation_to_speaker = {{0, 0}, {1, 1}};
  estimate.assignments_respect_cannot_link = true;
  assert(should_use_fragmented_speaker_fallback(
      segments, 6, 12, estimate));
}

void test_fragmented_speaker_fallback_rejects_balanced_tracks() {
  using namespace svp::audio::sherpa_diarization_internal;
  std::vector<svp::audio::SherpaDiarizationSegment> segments = {
      {0.0f, 20.0f, 0},
      {20.0f, 40.0f, 1},
      {40.0f, 60.0f, 2},
      {60.0f, 80.0f, 3},
      {80.0f, 100.0f, 4},
  };
  SpeakerCountEstimate estimate;
  estimate.speaker_count = 2;
  estimate.observation_to_speaker = {{0, 0}, {1, 1}};
  estimate.assignments_respect_cannot_link = true;
  assert(!should_use_fragmented_speaker_fallback(
      segments, 5, 10, estimate));
}

void test_fragmented_speaker_fallback_rejects_dense_observation_history() {
  using namespace svp::audio::sherpa_diarization_internal;
  std::vector<svp::audio::SherpaDiarizationSegment> segments = {
      {0.0f, 80.0f, 0},
      {80.0f, 84.0f, 1},
      {84.0f, 88.0f, 2},
      {88.0f, 92.0f, 3},
      {92.0f, 96.0f, 4},
      {96.0f, 100.0f, 5},
  };
  SpeakerCountEstimate estimate;
  estimate.speaker_count = 2;
  estimate.observation_to_speaker = {{0, 0}, {1, 1}};
  estimate.assignments_respect_cannot_link = true;
  assert(!should_use_fragmented_speaker_fallback(
      segments, 6, 13, estimate));
}
