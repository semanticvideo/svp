#pragma once

#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/remote_stream.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace svp::exec::remote {

// Registering a Bonjour name takes three probes 250 ms apart and an
// announcement (RFC 6762 §8), plus another round when the name conflicts and
// is renamed; 5 s covers that with margin. A listener that cannot advertise
// within it is reported as failed rather than left undiscoverable.
inline constexpr std::chrono::milliseconds kDefaultAdvertiseTimeout{5'000};

struct RemoteListenerOptions {
  PairingKey pairing;
  // Bonjour instance name. Empty lets the system use this Mac's computer
  // name. Bonjour renames on conflict; advertised_name() reports the result.
  // Coordinators find the worker by pairing id, not by this name.
  std::string service_name;
  // 0 lets the system choose a free port: coordinators learn it from
  // Bonjour, so it never needs to be fixed.
  std::uint16_t port = 0;
  TransportPolicy transport{};
  std::chrono::milliseconds advertise_timeout = kDefaultAdvertiseTimeout;
};

struct RemoteSessionInfo {
  // 1 for the first authenticated connection, then increasing.
  std::uint64_t session_number = 0;
  std::string peer;
};

// Serves one authenticated connection; runs on its own thread. The stream is
// cancelled when the handler returns.
using RemoteSessionHandler = std::function<void(RemoteStream&, const RemoteSessionInfo&)>;

// The worker side of the transport (plan §3.1, §3.3): listens on TCP with
// TLS 1.2 PSK and advertises Bonjour kWorkerServiceType with the pairing id
// in its TXT record. Connections negotiating kSessionAlpn go to the session
// handler; connections negotiating kRouteProbeAlpn are answered by the
// built-in throughput probe (route_policy.hpp) and never reach it. It only accepts connections; it never dials out, so it
// needs no Local Network permission in any launch context (plan §3.3,
// Apple TN3179). A peer that does not complete the handshake within
// transport.handshake_timeout, or proves a different secret, is dropped
// before the handler sees it.
class RemoteListener {
 public:
  RemoteListener(RemoteListenerOptions options, RemoteSessionHandler handler);
  ~RemoteListener();
  RemoteListener(const RemoteListener&) = delete;
  RemoteListener& operator=(const RemoteListener&) = delete;

  // Blocks until the listener accepts connections and its Bonjour service is
  // registered. Throws RemoteTransportError(invalid_configuration or
  // listener_failed).
  void start();

  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::string advertised_name() const;
  [[nodiscard]] std::size_t sessions_started() const;
  // Connections accepted whose handler has not returned yet.
  [[nodiscard]] std::size_t active_sessions() const;
  // Route probe connections answered (kRouteProbeAlpn); never sessions.
  [[nodiscard]] std::size_t route_probes_served() const;
  // Connections dropped because the TLS handshake failed or timed out, or
  // the peer asked for no protocol this listener serves.
  [[nodiscard]] std::size_t handshakes_rejected() const;

  // Stops advertising and accepting, cancels every session's connection, and
  // waits for every handler to return. Idempotent.
  void stop();

  struct Core;

 private:
  std::shared_ptr<Core> core_;
};

}  // namespace svp::exec::remote
