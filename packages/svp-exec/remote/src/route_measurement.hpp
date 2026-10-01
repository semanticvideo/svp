#pragma once

#include "nw_ref.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "svp/exec/remote/transport_policy.hpp"
#include "wait_signal.hpp"

#include <sys/socket.h>

#include <memory>
#include <optional>
#include <vector>

namespace svp::exec::remote::detail {

// One address to measure, pinned to the interface it was resolved on.
struct RouteTarget {
  NwRef<nw_interface_t> interface;
  sockaddr_storage address{};
  RouteCandidate route;
};

struct RouteMeasurement {
  std::vector<RouteProbe> probes;
  std::optional<std::size_t> winner;
  // True when some target failed in TLS (the peer does not hold the secret).
  bool any_tls_failure = false;
};

// Probes every target at once over its own kRouteProbeAlpn connection
// (route_probe_wire.hpp): handshake latency, then policy.probe_bytes each
// way. Returns as soon as select_route decides, or when every probe settled
// or the handshake timeout plus probe timeout passed; probes still connecting
// then count as failed and authenticated ones that did not finish as ready
// with no measured throughput. Throws RemoteTransportError(cancelled).
[[nodiscard]] RouteMeasurement measure_routes(const std::vector<RouteTarget>& targets,
                                              const PairingKey& key,
                                              const TransportPolicy& transport,
                                              const RoutePolicy& policy,
                                              const std::shared_ptr<WaitSignal>& signal);

// An unstarted connection to `address`, pinned to `interface` when set,
// speaking the given ALPN.
[[nodiscard]] NwRef<nw_connection_t> make_pinned_connection(
    const RouteTarget& target, const PairingKey& key, const TransportPolicy& transport,
    std::string_view application_protocol);

}  // namespace svp::exec::remote::detail
