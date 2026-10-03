#pragma once

#include "nw_ref.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "wait_signal.hpp"

#include <map>
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
  // Every TXT entry with a value.
  std::map<std::string, std::string> txt;
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

// Browses kWorkerServiceType for every service whose TXT record has
// `key`=`value`, however many there are: browsing ends once
// policy.discovery_settle passes without a new match, or at
// policy.discovery_timeout. Throws RemoteTransportError(cancelled,
// worker_not_found when browsing fails).
[[nodiscard]] std::vector<DiscoveredService> browse_for_txt(
    std::string_view key, std::string_view value, const RoutePolicy& policy,
    const std::shared_ptr<WaitSignal>& signal);

[[nodiscard]] RouteMedium route_medium_of(nw_interface_t interface);

}  // namespace svp::exec::remote::detail
