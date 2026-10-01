#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::remote {

// What kind of link an interface is, as the system classifies it. Thunderbolt
// networking and Ethernet are both `wired`.
enum class RouteMedium { loopback, wired, other, wifi, cellular };

[[nodiscard]] std::string_view route_medium_name(RouteMedium medium) noexcept;

// One way to reach a discovered worker: an address the worker's service
// resolved to on an interface it was discovered on. Everything here is
// reported by the system at run time; nothing is configured by name or
// address, and `address` and `link_rate_bps` are for reports only.
struct RouteCandidate {
  // Interface the route is pinned to. Empty when discovery reported no
  // interface; the system then picks one.
  std::string interface_name;
  std::string address;
  RouteMedium medium = RouteMedium::other;
  // Link rate the interface reports, in bits per second; 0 when unknown.
  std::uint64_t link_rate_bps = 0;
};

enum class RouteProbeState {
  // Still connecting or measuring.
  pending,
  // Authenticated. Throughput is measured when the probe finished in time,
  // and 0 when it did not.
  ready,
  // The handshake failed or timed out.
  failed,
};

// A measurement of one candidate (plan §3.4 "the coordinator measures"). All
// candidates are probed at the same time over their own authenticated
// connections.
struct RouteProbe {
  RouteCandidate candidate;
  RouteProbeState state = RouteProbeState::pending;
  // Connection start until TLS was ready.
  std::chrono::microseconds handshake{0};
  // Coordinator-to-worker and worker-to-coordinator payload rates over the
  // probe; 0 until measured.
  double upload_bytes_per_second = 0.0;
  double download_bytes_per_second = 0.0;
  // While pending after its handshake: how long the probe has been
  // measuring. Moving probe_bytes each way in that time or longer bounds its
  // throughput by 2 x probe_bytes / measuring, which lets a decision stop
  // waiting for a probe that can no longer win.
  std::chrono::microseconds measuring{0};
  std::string failure;

  // The slower direction: what a route sustains for transfers both ways.
  [[nodiscard]] double throughput() const;
};

// Plan §3.4: "Bonjour resolves every interface. The coordinator measures and
// prefers wired routes; Wi-Fi is used only if nothing else answers."
//
//   tiers  media in preference order. A route in an earlier tier wins over
//          any route in a later tier, whatever it measures: plan §3.4
//          measured Thunderbolt about 0.51 ms and Ethernet about 0.63 ms RTT
//          against Wi-Fi 28-38 ms with spikes over 100 ms. Loopback is its
//          own first tier (a worker on this Mac). Media in no tier are never
//          used.
//   Within a tier the route with the highest measured throughput (the
//   slower of its two directions) wins; ties go to the lowest handshake
//   latency. Reported link rates are not trusted: on Macs whose Thunderbolt
//   links report more than 4 Gb/s, Network framework TCP measured about
//   1.3 MB/s coordinator-to-worker over Thunderbolt while gigabit Ethernet
//   between the same Macs carried 115 MB/s both ways.
//
//   discovery_timeout 5 s: mDNS repeats an unanswered query after 1 s and
//          then at doubling intervals (RFC 6762 §5.2), so 5 s covers three
//          queries; a worker not found by then is reported as not found. The
//          same bound applies again to resolving its addresses.
//   discovery_settle 500 ms: after the pairing first answers, keep browsing
//          this long to learn the other interfaces it answers on, and after an
//          interface's first address, wait at most this long for the other
//          address family. A responder may delay answers 20-120 ms
//          (RFC 6762 §6) on each interface; 500 ms covers that plus the Wi-Fi
//          latency spikes measured in plan §3.4.
//   probe_bytes 4 MiB each way: large enough that TCP leaves slow start on a
//          LAN and per-connection setup does not dominate, so the probe ranks
//          sustained throughput; small enough that a healthy wired route
//          finishes in tens of milliseconds (4 MiB at gigabit Ethernet's
//          115 MB/s is 36 ms each way).
//   probe_timeout 1 s after each probe's handshake: a route that cannot move
//          probe_bytes both ways in a second (about 8 MB/s) is too slow for
//          blob transfer (plan §3.7) to matter; it stays usable, ranked as
//          unmeasured, and the bound keeps connect time predictable. A probe
//          that provably cannot beat the best measured route is not waited
//          for at all (RouteProbe::measuring).
inline constexpr std::chrono::milliseconds kDefaultDiscoveryTimeout{5'000};
inline constexpr std::chrono::milliseconds kDefaultDiscoverySettle{500};
inline constexpr std::uint64_t kDefaultRouteProbeBytes = 4ULL * 1024ULL * 1024ULL;
inline constexpr std::chrono::milliseconds kDefaultRouteProbeTimeout{1'000};

struct RoutePolicy {
  std::vector<std::vector<RouteMedium>> tiers = {
      {RouteMedium::loopback},
      {RouteMedium::wired, RouteMedium::other},
      {RouteMedium::wifi},
      {RouteMedium::cellular},
  };
  std::chrono::milliseconds discovery_timeout = kDefaultDiscoveryTimeout;
  std::chrono::milliseconds discovery_settle = kDefaultDiscoverySettle;
  std::uint64_t probe_bytes = kDefaultRouteProbeBytes;
  std::chrono::milliseconds probe_timeout = kDefaultRouteProbeTimeout;
};

// Throws RemoteTransportError(invalid_configuration) unless there is at least
// one tier, no medium appears twice, every duration is positive, and
// probe_bytes is between 1 and kMaxRouteProbeBytes (transport_policy.hpp).
void validate_route_policy(const RoutePolicy& policy);

// Index of the tier holding `medium`, or nullopt when the policy never uses it.
[[nodiscard]] std::optional<std::size_t> route_tier(const RoutePolicy& policy,
                                                    RouteMedium medium);

// Picks the winning probe, or returns nullopt when the choice is not decided
// yet. A pending probe can still win while it is in a better tier than the
// best ready probe, or in the same tier and either still connecting or
// measuring for so short a time that its throughput bound (see
// RouteProbe::measuring) exceeds the best measured throughput. With `final`
// (every probe settled or every deadline passed) the best ready probe wins,
// and nullopt means no route authenticated.
[[nodiscard]] std::optional<std::size_t> select_route(const RoutePolicy& policy,
                                                      std::span<const RouteProbe> probes,
                                                      bool final);

// The route a connection uses and how it was chosen, for logs and tests.
struct RouteChoice {
  // Bonjour instance name that answered for the pairing.
  std::string service_name;
  RouteCandidate route;
  // The session connection's own handshake.
  std::chrono::microseconds handshake{0};
  // Every candidate measured and how it ended.
  std::vector<RouteProbe> probes;
};

}  // namespace svp::exec::remote
