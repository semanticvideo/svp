#pragma once

#include "wait_signal.hpp"

#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace svp::exec::remote::detail {

// Why the connector resolves Bonjour services itself instead of dialing the
// service endpoint: measured on this macOS, a TLS connection to a Bonjour
// service endpoint whose handshake is rejected (wrong pairing secret) never
// fails; the framework keeps retrying the resolved addresses until the caller
// gives up. A connection to a concrete address fails within milliseconds with
// the TLS error. So each (service, interface) is resolved to addresses here
// and every address is dialed on that interface.

struct ServiceOnInterface {
  std::string name;
  std::string type;
  std::string domain;
  // 0 lets the system resolve on any interface.
  std::uint32_t interface_index = 0;
};

struct ResolvedAddress {
  sockaddr_storage address{};
  // Numeric host and port, for reports only.
  std::string text;
};

// Resolves every request concurrently with dns_sd (SRV, then A and AAAA on
// the same interface). A request is settled once it has both an IPv4 and an
// IPv6 answer, or `settle` after its first address, or on error. Returns
// when every request settled, at `timeout`, or throws
// RemoteTransportError(cancelled). One address list per request, possibly
// empty.
[[nodiscard]] std::vector<std::vector<ResolvedAddress>> resolve_services(
    const std::vector<ServiceOnInterface>& requests, std::chrono::milliseconds timeout,
    std::chrono::milliseconds settle, const std::shared_ptr<WaitSignal>& signal);

}  // namespace svp::exec::remote::detail
