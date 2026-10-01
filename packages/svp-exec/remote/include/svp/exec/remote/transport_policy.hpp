#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace svp::exec::remote {

// Plan §3.4: workers advertise this Bonjour service type in the local domain.
inline constexpr std::string_view kWorkerServiceType = "_svp-worker._tcp";
inline constexpr std::string_view kWorkerServiceDomain = "local.";
// TXT record key whose value is the worker's pairing id.
inline constexpr std::string_view kPairingTxtKey = "pairing";

// TLS application protocols (ALPN) that keep the two kinds of connection to
// a worker apart: worker sessions run the framed protocol (plan §4.3); route
// probes only measure a route's throughput (route_policy.hpp) and never
// reach the worker loop.
inline constexpr std::string_view kSessionAlpn = "svp-worker/1";
inline constexpr std::string_view kRouteProbeAlpn = "svp-route-probe/1";

// A worker answers a route probe of at most this many bytes each way and
// drops larger requests, so an authenticated peer cannot make it stream an
// unbounded amount. 64 MiB is sixteen default probes.
inline constexpr std::uint64_t kMaxRouteProbeBytes = 64ULL * 1024ULL * 1024ULL;

// Plan §3.4, measured on this macOS: Network framework does not accept TLS
// 1.3 external PSKs, so the transport pins TLS 1.2 with the single suite
// TLS_PSK_WITH_AES_128_GCM_SHA256. Both sides check the negotiated values
// after the handshake. Wire values from RFC 5246 and RFC 5487.
inline constexpr std::uint16_t kTls12ProtocolVersion = 0x0303;
inline constexpr std::uint16_t kTlsPskWithAes128GcmSha256 = 0x00A8;

// Why the defaults are what they are:
//   * handshake_timeout 5 s: TCP plus a TLS 1.2 PSK handshake is three round
//     trips. The slowest route measured in plan §3.4 is Wi-Fi at 28-38 ms
//     with spikes over 100 ms, so a healthy handshake finishes well under a
//     second; 5 s tolerates a busy peer while bounding how long a black-holed
//     interface, a stopped process, or a client that never speaks TLS can
//     hold a connection attempt or an accepted socket.
//   * keepalive 10 s idle, then 3 probes 5 s apart: a peer that vanishes
//     without closing (cable pulled, machine powered off) is detected in
//     about 25 s even when no frame is flowing, inside one default lease
//     floor (30 s, lease_policy.hpp). While leases run, heartbeats keep the
//     connection busy and lease expiry is the coordinator's detector.
inline constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{5'000};
inline constexpr std::chrono::seconds kDefaultKeepaliveIdle{10};
inline constexpr std::chrono::seconds kDefaultKeepaliveInterval{5};
inline constexpr std::uint32_t kDefaultKeepaliveProbes = 3;

// Connection behaviour shared by the worker listener and the coordinator
// connector. Both sides should use the same values.
struct TransportPolicy {
  std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout;
  std::chrono::seconds keepalive_idle = kDefaultKeepaliveIdle;
  std::chrono::seconds keepalive_interval = kDefaultKeepaliveInterval;
  std::uint32_t keepalive_probes = kDefaultKeepaliveProbes;
};

// Throws RemoteTransportError(invalid_configuration) unless every duration
// is positive and keepalive_probes >= 1.
void validate_transport_policy(const TransportPolicy& policy);

}  // namespace svp::exec::remote
