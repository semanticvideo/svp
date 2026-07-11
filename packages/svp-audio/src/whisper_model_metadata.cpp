#include "svp/audio/whisper_model_metadata.hpp"

#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

int required_integer(const std::map<std::string, std::string>& metadata,
                     const std::string& key) {
  const auto found = metadata.find(key);
  if (found == metadata.end()) {
    throw std::runtime_error("Whisper model metadata is missing " + key);
  }
  return std::stoi(found->second);
}

std::vector<int> required_integer_list(
    const std::map<std::string, std::string>& metadata,
    const std::string& key) {
  const auto found = metadata.find(key);
  if (found == metadata.end()) {
    throw std::runtime_error("Whisper model metadata is missing " + key);
  }

  std::vector<int> values;
  std::istringstream input(found->second);
  std::string value;
  while (std::getline(input, value, ',')) {
    if (!value.empty()) values.push_back(std::stoi(value));
  }
  if (values.empty()) {
    throw std::runtime_error("Whisper model metadata has an empty " + key);
  }
  return values;
}

}  // namespace

WhisperControlTokens parse_whisper_control_tokens(
    const std::map<std::string, std::string>& metadata) {
  WhisperControlTokens tokens;
  tokens.sot_sequence = required_integer_list(metadata, "sot_sequence");
  tokens.eot = required_integer(metadata, "eot");
  tokens.no_speech = required_integer(metadata, "no_speech");
  tokens.no_timestamps = required_integer(metadata, "no_timestamps");
  tokens.translate = required_integer(metadata, "translate");
  tokens.blank = required_integer(metadata, "blank_id");
  return tokens;
}

}  // namespace svp::audio
