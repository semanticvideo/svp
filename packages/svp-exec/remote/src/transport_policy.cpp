#include "svp/exec/remote/transport_policy.hpp"

#include "svp/exec/remote/remote_error.hpp"

namespace svp::exec::remote {

void validate_transport_policy(const TransportPolicy& policy) {
  if (policy.handshake_timeout.count() <= 0 || policy.keepalive_idle.count() <= 0 ||
      policy.keepalive_interval.count() <= 0 || policy.keepalive_probes == 0) {
    throw RemoteTransportError(
        RemoteErrorCode::invalid_configuration,
        "transport policy durations must be positive and keepalive_probes >= 1");
  }
}

}  // namespace svp::exec::remote
