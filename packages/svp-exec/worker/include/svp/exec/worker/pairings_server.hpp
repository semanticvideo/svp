#pragma once

// Every pairing of a worker on ONE TLS-PSK listener with ONE Bonjour
// instance of its own (plan §3.4), however many pairings it has:
//
//   - the listener holds every pairing's secret; the TLS stack picks the
//     PSK by the identity the coordinator sends (its pairing id);
//   - the worker's own instance is named after the Mac and advertises
//     worker=<worker id> (worker_identity.hpp); coordinators that know the
//     id (from HELLO_ACK) find the worker by it;
//   - for coordinators that predate worker ids, one more instance per
//     pairing, named after the pairing id (so no two collide and none is
//     renamed) and advertising pairing=<id> on the same port; they find the
//     worker exactly as before;
//   - which pairing a session serves is proven in HELLO (pairing_proof.hpp)
//     and checked against the secrets held, never trusted from the TLS
//     identity (the worker cannot read it) or from the claim alone;
//   - a pairing added or removed while serving (fleet join, `workers pair`,
//     `workers unpair`) takes effect at once: the listener restarts with
//     the new key set on its port, sessions in flight go on, and only the
//     advertisements that changed are registered again.
//
// Advertisements are registered with service_advertiser.hpp: off the
// serving path and retried with backoff, so a slow or failed registration
// never stops the worker from serving.

#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/remote/service_advertiser.hpp"
#include "svp/exec/worker/agent_session.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace svp::exec::worker {

using PairingSessionHandler = std::function<void(
    svp::exec::remote::RemoteStream& stream, const svp::exec::remote::RemoteSessionInfo& info,
    const CoordinatorResolver& resolve)>;

struct PairingsServerOptions {
  std::string worker_id;
  // Serves one authenticated session; `resolve` decides its coordinator from
  // its HELLO.
  PairingSessionHandler serve;
  std::function<void(const std::string&)> log;
  // The per-pairing instances for coordinators that predate worker ids.
  bool advertise_each_pairing = true;
  // Empty: this Mac's computer name.
  std::string service_name;
  svp::exec::remote::TransportPolicy transport{};
  // Runs before the listener is opened or its keys replaced; throwing makes
  // that change fail as a port that cannot be bound would (tests).
  std::function<void()> before_listener_change;
};

class PairingsServer {
 public:
  explicit PairingsServer(PairingsServerOptions options);
  ~PairingsServer();
  PairingsServer(const PairingsServer&) = delete;
  PairingsServer& operator=(const PairingsServer&) = delete;

  // Serves exactly `pairings` from now on (none: stops listening and
  // advertising). Throws RemoteTransportError(listener_failed) only when no
  // port could be listened on; it then serves nothing and holds the keys it
  // had before, so the next call opens a listener again.
  void set_pairings(const std::vector<svp::exec::remote::PairingKey>& pairings);

  // 0 while not listening.
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::size_t pairing_count() const;
  [[nodiscard]] std::size_t sessions_started() const;
  // Advertisements currently held (the worker's own and per-pairing ones).
  [[nodiscard]] std::vector<std::string> advertised_instances() const;
  // Blocks until every advertisement held is registered, or `timeout`.
  bool wait_advertised(std::chrono::milliseconds timeout) const;

  // Stops listening and advertising; open sessions end. Idempotent.
  void stop();

 private:
  void serve(svp::exec::remote::RemoteStream& stream,
             const svp::exec::remote::RemoteSessionInfo& info);
  [[nodiscard]] std::string resolve(const CoordinatorHello& hello,
                                    const std::vector<std::byte>& exporter) const;
  void advertise_all(std::uint16_t port);
  void open_or_replace_listener(const svp::exec::remote::PairingKey& first,
                                const std::vector<svp::exec::remote::PairingKey>& accepted);

  PairingsServerOptions options_;
  mutable std::mutex mutex_;
  // pairing id -> key; read by sessions, replaced by set_pairings.
  std::map<std::string, svp::exec::remote::PairingKey> keys_;
  std::mutex changing_;
  std::unique_ptr<svp::exec::remote::RemoteListener> listener_;
  std::unique_ptr<svp::exec::remote::ServiceAdvertiser> own_advertisement_;
  std::map<std::string, std::unique_ptr<svp::exec::remote::ServiceAdvertiser>> pairing_advertisements_;
};

}  // namespace svp::exec::worker
