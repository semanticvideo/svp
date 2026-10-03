#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace svp::exec {

// RC2 §5.15 rule 1: BLAKE3 output is exactly 32 bytes.
inline constexpr std::size_t kBlake3DigestBytes = 32;
// RC2 §5.15 rule 2: JSON records store BLAKE3 as 64 lowercase hex characters.
inline constexpr std::size_t kBlake3HexChars = kBlake3DigestBytes * 2;
// RC2 §20.2 writes `cache_key` as "b3:<hex>"; the distributed plan (§4.2) uses
// the same form for `output_digest` and `runtime_id`. Plain `blake3` and
// `*_blake3` fields stay bare hex.
inline constexpr std::string_view kB3DigestPrefix = "b3:";

using Blake3Digest = std::array<std::uint8_t, kBlake3DigestBytes>;

[[nodiscard]] Blake3Digest blake3_digest(std::span<const std::byte> bytes);
[[nodiscard]] Blake3Digest blake3_digest(std::string_view bytes);

// BLAKE3 key derivation (derive_key mode): a 32-byte key for `context`, a
// fixed, versioned, application-specific string, from `material`.
[[nodiscard]] Blake3Digest blake3_derive_key(std::string_view context,
                                             std::span<const std::byte> material);
// BLAKE3 keyed hash (MAC) of `bytes` under a 32-byte `key`.
[[nodiscard]] Blake3Digest blake3_keyed_hash(const Blake3Digest& key,
                                             std::span<const std::byte> bytes);

// 64 lowercase hex characters.
[[nodiscard]] std::string blake3_hex(const Blake3Digest& digest);
// Accepts exactly 64 lowercase hex characters; anything else is nullopt.
[[nodiscard]] std::optional<Blake3Digest> parse_blake3_hex(std::string_view hex);

// "b3:" followed by 64 lowercase hex characters.
[[nodiscard]] std::string blake3_prefixed(const Blake3Digest& digest);
[[nodiscard]] std::optional<Blake3Digest> parse_blake3_prefixed(
    std::string_view value);

}  // namespace svp::exec
