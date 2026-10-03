#pragma once

#include "svp/exec/byte_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::remote {

// What the TLS handshake negotiated, as wire values (see transport_policy.hpp
// for the only accepted pair).
struct TlsSession {
  std::uint16_t protocol_version = 0;
  std::uint16_t cipher_suite = 0;
  // Negotiated ALPN (kSessionAlpn or kRouteProbeAlpn); empty when none.
  std::string application_protocol;

  bool operator==(const TlsSession&) const = default;
};

// An established connection to a paired peer, authenticated by TLS 1.2 PSK
// (plan §3.4, §4.1 `pairing`). Frames run over it through StreamFrameReader
// and StreamFrameWriter exactly as they do over a socket pair.
//
// There is no half-close: measured on this macOS, Network framework TLS
// completes a final-context send but the peer never reads end of stream,
// while cancel() does reach it as end of stream. Sessions therefore end with
// the protocol's SHUTDOWN frame and then cancel().
//
// A connection that fails (for example the peer resets it) is torn down at
// once: a write in flight throws ExecError(frame_truncated) rather than
// waiting for a cancel() that may never come.
class RemoteStream : public ByteStream {
 public:
  // Tears the connection down: a blocked read_some returns 0 and later
  // writes throw ExecError(frame_truncated). Idempotent and callable from any
  // thread.
  virtual void cancel() = 0;

  [[nodiscard]] virtual TlsSession tls_session() const = 0;

  // The peer's address as the system reports it, for logs only; nothing may
  // select or trust a peer by it.
  [[nodiscard]] virtual std::string peer_description() const = 0;

  // TLS keying material exporter (RFC 5705) of this connection: `bytes`
  // bytes, the same on both ends, unique to this connection and derived from
  // its PSK. Lets a peer prove in-band which pairing secret it holds, bound
  // to this connection (the worker cannot read the negotiated PSK identity).
  // nullopt when the connection is not ready or offers no exporter.
  [[nodiscard]] virtual std::optional<std::vector<std::byte>> export_keying_material(
      std::string_view label, std::size_t bytes) const {
    (void)label;
    (void)bytes;
    return std::nullopt;
  }
};

}  // namespace svp::exec::remote
