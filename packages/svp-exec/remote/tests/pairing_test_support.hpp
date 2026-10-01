#pragma once

// Test-only pairing material: random keys and a hex secret file format for
// the test worker and driver. Real pairing creation and storage belong to the
// pairing flow (plan §3.3), not to these helpers.

#include "svp/exec/remote/pairing_key.hpp"

#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace svp::exec::remote::test {

inline std::vector<std::byte> random_bytes(std::size_t count) {
  std::random_device device;
  std::vector<std::byte> bytes(count);
  for (std::byte& byte : bytes) {
    byte = static_cast<std::byte>(device() & 0xFFU);
  }
  return bytes;
}

inline std::string to_hex(const std::vector<std::byte>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const std::byte byte : bytes) {
    const auto value = static_cast<unsigned>(byte);
    text += kDigits[value >> 4U];
    text += kDigits[value & 0xFU];
  }
  return text;
}

inline std::vector<std::byte> parse_hex(std::string_view text) {
  std::string digits;
  for (const char character : text) {
    if (!std::isspace(static_cast<unsigned char>(character))) {
      digits += character;
    }
  }
  if (digits.size() % 2 != 0) {
    throw std::runtime_error("hex secret has an odd number of digits");
  }
  std::vector<std::byte> bytes;
  for (std::size_t index = 0; index < digits.size(); index += 2) {
    bytes.push_back(static_cast<std::byte>(std::stoul(digits.substr(index, 2), nullptr, 16)));
  }
  return bytes;
}

// A fresh pairing whose id is unique to this process and call.
inline PairingKey random_pairing(std::string_view label) {
  return PairingKey{.pairing_id = std::string(label) + "-" + std::to_string(::getpid()) + "-" +
                                  to_hex(random_bytes(4)),
                    .secret = random_bytes(kMinPairingSecretBytes)};
}

// Secret file: the secret as hex digits; whitespace is ignored.
inline PairingKey read_pairing_file(std::string pairing_id, const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot read secret file " + path);
  }
  std::stringstream text;
  text << file.rdbuf();
  return PairingKey{.pairing_id = std::move(pairing_id), .secret = parse_hex(text.str())};
}

inline void write_pairing_file(const std::string& path, const PairingKey& key) {
  std::ofstream(path) << to_hex(key.secret) << "\n";
}

}  // namespace svp::exec::remote::test
