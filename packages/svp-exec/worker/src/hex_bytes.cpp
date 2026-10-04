#include "hex_bytes.hpp"

#include <array>
#include <cstdint>

namespace svp::exec::worker::detail {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
// Bits per base64 character and per byte.
constexpr unsigned kBase64Bits = 6;
constexpr unsigned kByteBits = 8;

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

std::string bytes_hex(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size() * 2);
  for (const std::byte byte : bytes) {
    const auto value = static_cast<unsigned>(byte);
    text += kHexDigits[value >> 4U];
    text += kHexDigits[value & 0xFU];
  }
  return text;
}

std::optional<std::vector<std::byte>> parse_hex_bytes(std::string_view hex,
                                                      std::size_t expected_bytes) {
  if (hex.size() != expected_bytes * 2) {
    return std::nullopt;
  }
  std::vector<std::byte> bytes;
  bytes.reserve(expected_bytes);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const int high = hex_value(hex[index]);
    const int low = hex_value(hex[index + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<std::byte>(high * 16 + low));
  }
  return bytes;
}

std::string base64url_encode(std::string_view bytes) {
  std::string text;
  std::uint32_t buffer = 0;
  unsigned bits = 0;
  for (const char c : bytes) {
    buffer = (buffer << kByteBits) | static_cast<std::uint8_t>(c);
    bits += kByteBits;
    while (bits >= kBase64Bits) {
      bits -= kBase64Bits;
      text += kBase64UrlAlphabet[(buffer >> bits) & 0x3FU];
    }
  }
  if (bits > 0) {
    text += kBase64UrlAlphabet[(buffer << (kBase64Bits - bits)) & 0x3FU];
  }
  return text;
}

std::optional<std::string> base64url_decode(std::string_view text) {
  // A final group of one character cannot hold a whole byte.
  if (text.size() % 4 == 1) {
    return std::nullopt;
  }
  std::string bytes;
  std::uint32_t buffer = 0;
  unsigned bits = 0;
  for (const char c : text) {
    const std::size_t value = kBase64UrlAlphabet.find(c);
    if (value == std::string_view::npos) {
      return std::nullopt;
    }
    buffer = (buffer << kBase64Bits) | static_cast<std::uint32_t>(value);
    bits += kBase64Bits;
    if (bits >= kByteBits) {
      bits -= kByteBits;
      bytes += static_cast<char>((buffer >> bits) & 0xFFU);
    }
  }
  return bytes;
}

}  // namespace svp::exec::worker::detail
