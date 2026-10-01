#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace svp::exec::remote {

enum class RemoteErrorCode {
  // Pairing material or a policy value is unusable (checked before any
  // socket is opened).
  invalid_configuration,
  // The listener could not bind, start, or advertise itself.
  listener_failed,
  // No Bonjour service advertised this pairing id before the discovery
  // timeout.
  worker_not_found,
  // The pairing was found but no route finished the TLS handshake.
  no_route,
  // Every route that reached the peer failed TLS authentication: the peer
  // does not hold this pairing secret (plan §4.1 `pairing`: no connection).
  authentication_failed,
  // cancel() was called while connecting.
  cancelled,
};

[[nodiscard]] std::string_view remote_error_code_name(RemoteErrorCode code) noexcept;

class RemoteTransportError : public std::runtime_error {
 public:
  RemoteTransportError(RemoteErrorCode code, std::string message);
  [[nodiscard]] RemoteErrorCode code() const noexcept;

 private:
  RemoteErrorCode code_;
};

}  // namespace svp::exec::remote
