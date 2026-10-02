#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace svp::audio {

using DiarizationProgressCallback =
    std::function<void(std::size_t current, std::size_t total)>;

struct SherpaDiarizationSegment {
  float start_sec = 0.0f;
  float end_sec = 0.0f;
  int32_t speaker_id = 0;
};

}  // namespace svp::audio
