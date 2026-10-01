#pragma once

#include "nw_ref.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "wait_signal.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::remote::detail {

// One Bonjour service instance whose TXT record carries the wanted pairing
// id, with every interface the system saw it on.
struct DiscoveredService {
  std::string name;
  std::string type;
  std::string domain;
  std::vector<NwRef<nw_interface_t>> interfaces;
};

// Browses kWorkerServiceType in kWorkerServiceDomain for services whose TXT
// record names `pairing_id` (plan §3.4: never by host name or address). Once
// the first match appears, browsing continues for policy.discovery_settle to
// collect the other interfaces it answers on. Returns every match, or an
// empty list when none appeared within policy.discovery_timeout. Throws
// RemoteTransportError(cancelled) when `signal` is cancelled.
[[nodiscard]] std::vector<DiscoveredService> browse_for_pairing(
    std::string_view pairing_id, const RoutePolicy& policy,
    const std::shared_ptr<WaitSignal>& signal);

[[nodiscard]] RouteMedium route_medium_of(nw_interface_t interface);

}  // namespace svp::exec::remote::detail
