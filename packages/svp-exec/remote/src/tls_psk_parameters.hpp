#pragma once

#include "nw_ref.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/remote_stream.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <string>
#include <vector>

namespace svp::exec::remote::detail {

// TCP + TLS parameters shared by the listener and every outgoing connection
// (plan §3.4): TLS 1.2 only, the single suite TLS_PSK_WITH_AES_128_GCM_SHA256,
// the pairing secret as the PSK and the pairing id as its identity, and the
// given ALPN protocols (a listener offers every protocol it serves, a client
// the one it wants); TCP with Nagle off (heartbeats are small frames that
// must not wait for an ACK) and keepalive from `policy`. Peer-to-peer (AWDL)
// interfaces are excluded: workers are reached over the LAN only.
[[nodiscard]] NwRef<nw_parameters_t> make_tls_psk_parameters(
    const PairingKey& key, const TransportPolicy& policy,
    const std::vector<std::string>& application_protocols);

// A listener's parameters accepting every key of `keys` (non-empty, unique
// ids): the TLS stack picks the PSK by the identity the client sends.
[[nodiscard]] NwRef<nw_parameters_t> make_tls_psk_parameters(
    const std::vector<PairingKey>& keys, const TransportPolicy& policy,
    const std::vector<std::string>& application_protocols);

// Reads the negotiated TLS version and suite from a ready connection.
[[nodiscard]] TlsSession read_tls_session(nw_connection_t connection);

// True for the only TLS version and suite this transport accepts (the ALPN is
// checked by whoever dispatches on it).
[[nodiscard]] bool is_expected_tls_session(const TlsSession& session);

// "<domain> error <code>: <system description>" for logs and exceptions.
[[nodiscard]] std::string describe_nw_error(nw_error_t error);

}  // namespace svp::exec::remote::detail
