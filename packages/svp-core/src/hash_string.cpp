#include "svp/core/hash_string.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace svp::core {

std::string_view hash_algorithm_name(HashAlgorithm algorithm) noexcept {
  switch (algorithm) {
    case HashAlgorithm::blake3:
      return "blake3";
  }

  return "unknown";
}

HashString::HashString(HashAlgorithm algorithm, std::string hex_value)
    : algorithm_(algorithm), hex_value_(std::move(hex_value)) {}

HashAlgorithm HashString::algorithm() const noexcept {
  return algorithm_;
}

const std::string& HashString::hex_value() const noexcept {
  return hex_value_;
}

std::string HashString::canonical() const {
  std::string value(hash_algorithm_name(algorithm_));
  value.push_back(':');
  value += hex_value_;
  return value;
}

bool is_lower_hex(std::string_view value) noexcept {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isdigit(character) != 0 ||
           (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('f'));
  });
}

std::optional<HashString> parse_hash_string(std::string_view value) {
  constexpr std::string_view prefix = "blake3:";

  if (!value.starts_with(prefix)) {
    return std::nullopt;
  }

  std::string hex_value(value.substr(prefix.size()));
  if (hex_value.empty() || !is_lower_hex(hex_value)) {
    return std::nullopt;
  }

  return HashString(HashAlgorithm::blake3, std::move(hex_value));
}

}  // namespace svp::core
