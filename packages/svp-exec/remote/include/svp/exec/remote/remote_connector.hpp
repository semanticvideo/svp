#pragma once

#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/remote_stream.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <memory>
#include <string>
#include <vector>

namespace svp::exec::remote {

struct RemoteConnectorOptions {
  PairingKey pairing;
  RoutePolicy routes{};
  TransportPolicy transport{};
};

// A Bonjour service that advertises the pairing, with the routes to it.
struct DiscoveredWorker {
  std::string service_name;
  std::vector<RouteCandidate> candidates;
};

struct RemoteConnection {
  std::unique_ptr<RemoteStream> stream;
  RouteChoice route;
};

// The coordinator side of the transport (plan §3.4): finds a worker by
// pairing id through Bonjour, never by host name or address; resolves the
// service to its addresses on every interface it answered on; measures every
// route at once with an authenticated throughput probe (route_policy.hpp);
// and opens the session connection on the route RoutePolicy prefers. Dialing
// out requires Local Network permission unless the process is a command-line
// tool run from Terminal or SSH, a launchd daemon, or root (plan §3.3,
// Apple TN3179).
class RemoteConnector {
 public:
  explicit RemoteConnector(RemoteConnectorOptions options);
  ~RemoteConnector();
  RemoteConnector(const RemoteConnector&) = delete;
  RemoteConnector& operator=(const RemoteConnector&) = delete;

  // Every service advertising the pairing, with each address it resolved to
  // on each interface the route policy allows. Throws RemoteTransportError
  // (worker_not_found, cancelled).
  [[nodiscard]] std::vector<DiscoveredWorker> discover();

  // Discovers the pairing, measures its routes, and returns an authenticated
  // session connection (kSessionAlpn) over the preferred route. Throws
  // RemoteTransportError: worker_not_found; no_route when no route
  // authenticated; authentication_failed when a route failed TLS (the peer
  // does not hold this secret); cancelled.
  [[nodiscard]] RemoteConnection connect();

  // Makes a discover() or connect() in progress, and every later call,
  // throw RemoteTransportError(cancelled). Thread-safe.
  void cancel();

  struct State;

 private:
  std::shared_ptr<State> state_;
};

}  // namespace svp::exec::remote
