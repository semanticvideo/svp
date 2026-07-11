#pragma once

#include <map>
#include <string>
#include <vector>

namespace svp::audio {

struct WhisperControlTokens {
  std::vector<int> sot_sequence;
  int eot = 0;
  int no_speech = 0;
  int no_timestamps = 0;
  int translate = 0;
  int blank = 0;

  [[nodiscard]] int timestamp_begin() const noexcept {
    return no_timestamps + 1;
  }
};

[[nodiscard]] WhisperControlTokens parse_whisper_control_tokens(
    const std::map<std::string, std::string>& metadata);

}  // namespace svp::audio
