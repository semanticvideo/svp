#include "audio_test_support.hpp"
#include "../src/sherpa_diarization/speaker_count_estimator.hpp"

void test_spectral_speaker_count_estimator_finds_three_clusters() {
  using namespace svp::audio::sherpa_diarization_internal;
  const std::vector<SpeakerObservation> observations = {
      {0, 0.0f, {1.0f, 0.0f, 0.0f}},
      {1, 1.0f, {0.99f, 0.01f, 0.0f}},
      {2, 2.0f, {0.0f, 1.0f, 0.0f}},
      {3, 3.0f, {0.01f, 0.99f, 0.0f}},
      {4, 4.0f, {0.0f, 0.0f, 1.0f}},
      {5, 5.0f, {0.0f, 0.01f, 0.99f}},
  };
  const SpeakerCountEstimate estimate =
      estimate_speaker_count(observations, {});
  if (estimate.speaker_count != 3) {
    std::string values;
    for (double value : estimate.laplacian_eigenvalues) {
      if (!values.empty()) values += ",";
      values += std::to_string(value);
    }
    throw std::runtime_error(
        "spectral three-cluster estimate was " +
        std::to_string(estimate.speaker_count) + " with eigenvalues " +
        values);
  }
  assert(estimate.observation_to_speaker.at(0) ==
         estimate.observation_to_speaker.at(1));
  assert(estimate.observation_to_speaker.at(2) ==
         estimate.observation_to_speaker.at(3));
  assert(estimate.observation_to_speaker.at(4) ==
         estimate.observation_to_speaker.at(5));
}

void test_spectral_speaker_count_estimator_preserves_cannot_link_pair() {
  using namespace svp::audio::sherpa_diarization_internal;
  const std::vector<SpeakerObservation> observations = {
      {0, 0.0f, {1.0f, 0.0f}},
      {1, 0.0f, {0.99f, 0.01f}},
  };
  const SpeakerCountEstimate estimate =
      estimate_speaker_count(observations, {{0, 1}});
  assert(estimate.speaker_count == 2);
  assert(estimate.assignments_respect_cannot_link);
  assert(estimate.observation_to_speaker.at(0) !=
         estimate.observation_to_speaker.at(1));
}
