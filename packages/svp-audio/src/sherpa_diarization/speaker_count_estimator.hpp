#pragma once

#include "private.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace svp::audio::sherpa_diarization_internal {

struct SpeakerCountEstimate {
  std::size_t speaker_count = 0;
  std::vector<double> laplacian_eigenvalues;
  double selected_eigengap = 0.0;
  std::map<int32_t, int32_t> observation_to_speaker;
  bool assignments_respect_cannot_link = true;
};

[[nodiscard]] SpeakerCountEstimate estimate_speaker_count(
    const std::vector<SpeakerObservation>& observations,
    const std::set<std::pair<int32_t, int32_t>>& cannot_link_observations);

}  // namespace svp::audio::sherpa_diarization_internal
