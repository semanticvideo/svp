#include "svp/exec/blake3_digest.hpp"

#include "svp/core/hash_string.hpp"

#include <blake3.h>

static_assert(BLAKE3_OUT_LEN == svp::exec::kBlake3DigestBytes,
              "BLAKE3 library output length must match RC2 §5.15");

namespace svp::exec {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  return character - 'a' + 10;
}

}  // namespace

Blake3Digest blake3_digest(std::span<const std::byte> bytes) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
  Blake3Digest digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return digest;
}

Blake3Digest blake3_digest(std::string_view bytes) {
  return blake3_digest(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

std::string blake3_hex(const Blake3Digest& digest) {
  std::string hex;
  hex.reserve(kBlake3HexChars);
  for (const std::uint8_t byte : digest) {
    hex.push_back(kHexDigits[byte >> 4U]);
    hex.push_back(kHexDigits[byte & 0x0FU]);
  }
  return hex;
}

std::optional<Blake3Digest> parse_blake3_hex(std::string_view hex) {
  if (hex.size() != kBlake3HexChars || !svp::core::is_lower_hex(hex)) {
    return std::nullopt;
  }
  Blake3Digest digest{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    digest[index] = static_cast<std::uint8_t>(
        (hex_value(hex[index * 2]) << 4) | hex_value(hex[index * 2 + 1]));
  }
  return digest;
}

std::string blake3_prefixed(const Blake3Digest& digest) {
  std::string value(kB3DigestPrefix);
  value += blake3_hex(digest);
  return value;
}

std::optional<Blake3Digest> parse_blake3_prefixed(std::string_view value) {
  if (!value.starts_with(kB3DigestPrefix)) {
    return std::nullopt;
  }
  return parse_blake3_hex(value.substr(kB3DigestPrefix.size()));
}

}  // namespace svp::exec
