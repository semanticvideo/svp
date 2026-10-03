#pragma once

// Lowercase hex and unpadded base64url (RFC 4648 §5) for fleet keys and
// tokens.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker::detail {

[[nodiscard]] std::string bytes_hex(std::span<const std::byte> bytes);
// Exactly `expected_bytes` bytes as lowercase hex; nullopt otherwise.
[[nodiscard]] std::optional<std::vector<std::byte>> parse_hex_bytes(std::string_view hex,
                                                                    std::size_t expected_bytes);

[[nodiscard]] std::string base64url_encode(std::string_view bytes);
// nullopt for a character outside the alphabet or a malformed length.
[[nodiscard]] std::optional<std::string> base64url_decode(std::string_view text);

}  // namespace svp::exec::worker::detail
