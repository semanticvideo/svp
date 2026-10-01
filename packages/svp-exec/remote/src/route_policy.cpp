#include "svp/exec/remote/route_policy.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <algorithm>

#include <set>
#include <tuple>

namespace svp::exec::remote {
namespace {

// Lower wins: (tier, faster measured throughput, quicker handshake).
using ProbeRank = std::tuple<std::size_t, double, std::chrono::microseconds>;

ProbeRank rank(const RoutePolicy& policy, const RouteProbe& probe) {
  const std::size_t tier = route_tier(policy, probe.candidate.medium).value_or(policy.tiers.size());
  return {tier, -probe.throughput(), probe.handshake};
}

}  // namespace

double RouteProbe::throughput() const {
  return std::min(upload_bytes_per_second, download_bytes_per_second);
}

std::string_view route_medium_name(RouteMedium medium) noexcept {
  switch (medium) {
    case RouteMedium::loopback:
      return "loopback";
    case RouteMedium::wired:
      return "wired";
    case RouteMedium::other:
      return "other";
    case RouteMedium::wifi:
      return "wifi";
    case RouteMedium::cellular:
      return "cellular";
  }
  return "unknown";
}

void validate_route_policy(const RoutePolicy& policy) {
  std::set<RouteMedium> seen;
  for (const auto& tier : policy.tiers) {
    if (tier.empty()) {
      throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                                 "route policy tiers must not be empty");
    }
    for (const RouteMedium medium : tier) {
      if (!seen.insert(medium).second) {
        throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                                   "route medium `" + std::string(route_medium_name(medium)) +
                                       "` appears in more than one tier");
      }
    }
  }
  if (seen.empty() || policy.discovery_timeout.count() <= 0 ||
      policy.discovery_settle.count() <= 0 || policy.probe_timeout.count() <= 0 ||
      policy.probe_bytes == 0 || policy.probe_bytes > kMaxRouteProbeBytes) {
    throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                               "route policy needs a tier, positive durations, and probe_bytes "
                               "in 1.." +
                                   std::to_string(kMaxRouteProbeBytes));
  }
}

std::optional<std::size_t> route_tier(const RoutePolicy& policy, RouteMedium medium) {
  for (std::size_t index = 0; index < policy.tiers.size(); ++index) {
    for (const RouteMedium member : policy.tiers[index]) {
      if (member == medium) {
        return index;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> select_route(const RoutePolicy& policy,
                                        std::span<const RouteProbe> probes, bool final) {
  std::optional<std::size_t> best;
  for (std::size_t index = 0; index < probes.size(); ++index) {
    const RouteProbe& probe = probes[index];
    if (probe.state != RouteProbeState::ready || !route_tier(policy, probe.candidate.medium)) {
      continue;
    }
    if (!best || rank(policy, probe) < rank(policy, probes[*best])) {
      best = index;
    }
  }
  if (!best || final) {
    return best;
  }
  const std::size_t best_tier = std::get<0>(rank(policy, probes[*best]));
  const double best_throughput = probes[*best].throughput();
  for (const RouteProbe& probe : probes) {
    const auto tier = route_tier(policy, probe.candidate.medium);
    if (probe.state != RouteProbeState::pending || !tier || *tier > best_tier) {
      continue;
    }
    if (*tier < best_tier || probe.measuring.count() <= 0) {
      return std::nullopt;
    }
    // Still measuring after `measuring`: both directions together took longer,
    // so the slower one ran at most 2 x probe_bytes / measuring.
    const double bound = 2.0 * static_cast<double>(policy.probe_bytes) /
                         std::chrono::duration<double>(probe.measuring).count();
    if (bound > best_throughput) {
      return std::nullopt;
    }
  }
  return best;
}

}  // namespace svp::exec::remote
