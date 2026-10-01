#pragma once

// Route probe wire format, on a connection whose ALPN is kRouteProbeAlpn:
//
//   coordinator -> worker   u64 big-endian N (1..kMaxRouteProbeBytes), N bytes
//   worker -> coordinator   N bytes, once all N have arrived
//
// The coordinator then cancels the connection. Payload bytes carry no
// meaning; only their timing does.

#include <array>
#include <cstddef>
#include <cstdint>

namespace svp::exec::remote::detail {

inline constexpr std::size_t kRouteProbeHeaderBytes = 8;
// Bytes per write/read while probing: small enough that the first byte of the
// reply is seen promptly, large enough to keep per-call overhead negligible.
inline constexpr std::size_t kRouteProbeChunkBytes = 1U * 1024U * 1024U;

inline std::array<std::byte, kRouteProbeHeaderBytes> encode_probe_length(std::uint64_t length) {
  std::array<std::byte, kRouteProbeHeaderBytes> header{};
  for (std::size_t index = 0; index < header.size(); ++index) {
    header[index] = static_cast<std::byte>(length >> (8U * (header.size() - 1 - index)));
  }
  return header;
}

inline std::uint64_t decode_probe_length(const std::array<std::byte, kRouteProbeHeaderBytes>& header) {
  std::uint64_t length = 0;
  for (const std::byte byte : header) {
    length = (length << 8U) | static_cast<std::uint64_t>(byte);
  }
  return length;
}

}  // namespace svp::exec::remote::detail
