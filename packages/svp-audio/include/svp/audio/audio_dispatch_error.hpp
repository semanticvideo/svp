#pragma once

#include <stdexcept>

namespace svp::audio {

// A dispatcher of audio work (ASR chunks, diarization windows) that cannot
// deliver it. The audio boundaries never turn it into a blocker: the build
// fails rather than writing a package that differs from a local build's.
class AudioDispatchError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace svp::audio
