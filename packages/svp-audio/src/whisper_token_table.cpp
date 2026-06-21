#include "svp/audio/whisper_token_table.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

std::string base64_decode(const std::string& encoded) {
  static const int decode_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  };

  std::string output;
  int val = 0, valb = -8;
  for (unsigned char c : encoded) {
    const int d = decode_table[c];
    if (d == -1) break;
    val = (val << 6) | d;
    valb += 6;
    if (valb >= 0) {
      output.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return output;
}

}  // namespace

bool WhisperTokenTable::load(const std::filesystem::path& tokens_path) {
  std::ifstream input(tokens_path);
  if (!input) return false;

  tokens_.clear();
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream iss(line);
    std::string encoded, index_str;
    if (!(iss >> encoded >> index_str)) {
      tokens_.push_back("");
      continue;
    }
    tokens_.push_back(base64_decode(encoded));
  }
  return true;
}

std::optional<std::string> WhisperTokenTable::token_text(int token_id) const {
  if (token_id < 0 || static_cast<std::size_t>(token_id) >= tokens_.size()) {
    return std::nullopt;
  }
  return tokens_[token_id];
}

std::size_t WhisperTokenTable::size() const noexcept {
  return tokens_.size();
}

}  // namespace svp::audio
