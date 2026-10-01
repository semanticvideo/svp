// Route selection rules (plan §3.4), independent of any real interface.

#include "exec_test_support.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/route_policy.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <vector>

namespace {

using namespace svp::exec::remote;
using svp::exec::test::expect;
using svp::exec::test::run_tests;
using std::chrono::microseconds;

// Throughputs in bytes per second, shaped like the measurements in
// route_policy.hpp: a fast link, gigabit Ethernet, and a link that is fast one
// way and nearly stalled the other.
constexpr double kFast = 1.0e9;
constexpr double kGigabit = 115.0e6;
constexpr double kStalled = 1.3e6;

RouteProbe probe(std::string name, RouteMedium medium, RouteProbeState state,
                 double upload = 0.0, double download = 0.0, std::int64_t handshake_us = 0) {
  RouteProbe result;
  result.candidate = RouteCandidate{.interface_name = std::move(name), .medium = medium};
  result.state = state;
  result.upload_bytes_per_second = upload;
  result.download_bytes_per_second = download;
  result.handshake = microseconds(handshake_us);
  return result;
}

void test_wired_beats_faster_wifi() {
  const std::vector probes{
      probe("wifi0", RouteMedium::wifi, RouteProbeState::ready, kFast, kFast, 100),
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 900),
  };
  expect(select_route(RoutePolicy{}, probes, false) == 1, "wired wins over a faster Wi-Fi");
}

void test_wifi_waits_for_pending_wired() {
  const std::vector probes{
      probe("wifi0", RouteMedium::wifi, RouteProbeState::ready, kGigabit, kGigabit, 100),
      probe("eth0", RouteMedium::wired, RouteProbeState::pending),
  };
  expect(!select_route(RoutePolicy{}, probes, false), "undecided while wired may answer");
  expect(select_route(RoutePolicy{}, probes, true) == 0, "Wi-Fi only when wired never answers");
}

void test_slower_direction_decides() {
  // Fast one way but stalled the other loses to a route that is merely
  // gigabit both ways.
  const std::vector probes{
      probe("tb0", RouteMedium::wired, RouteProbeState::ready, kStalled, kFast, 300),
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 600),
  };
  expect(select_route(RoutePolicy{}, probes, false) == 1, "bottleneck direction decides");
}

void test_pending_same_tier_is_awaited() {
  const std::vector probes{
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 400),
      probe("tb0", RouteMedium::wired, RouteProbeState::pending),
  };
  expect(!select_route(RoutePolicy{}, probes, false), "a same-tier probe may still measure faster");
}

void test_hopeless_pending_probe_is_not_awaited() {
  const RoutePolicy policy;
  RouteProbe slow = probe("tb0", RouteMedium::wired, RouteProbeState::pending);
  const std::vector measured{
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 400),
  };
  // Measuring for longer than the measured route needed both ways: the
  // pending probe can no longer reach its throughput, so it cannot win.
  const double both_ways = 2.0 * static_cast<double>(policy.probe_bytes) / kGigabit;
  slow.measuring =
      std::chrono::duration_cast<microseconds>(std::chrono::duration<double>(both_ways)) +
      microseconds(1);
  std::vector probes = measured;
  probes.push_back(slow);
  expect(select_route(policy, probes, false) == 0, "a probe that cannot win is not awaited");
  probes.back().measuring = probes.back().measuring / 4;
  expect(!select_route(policy, probes, false), "a probe that still could win is awaited");
}

void test_worse_tier_pending_does_not_delay() {
  const std::vector probes{
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 400),
      probe("wifi0", RouteMedium::wifi, RouteProbeState::pending),
  };
  expect(select_route(RoutePolicy{}, probes, false) == 0, "no need to wait for Wi-Fi");
}

void test_latency_breaks_throughput_ties() {
  const std::vector probes{
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 700),
      probe("eth1", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 500),
  };
  expect(select_route(RoutePolicy{}, probes, false) == 1, "lower handshake latency wins a tie");
}

void test_unmeasured_ranks_last_in_tier() {
  const std::vector probes{
      probe("tb0", RouteMedium::wired, RouteProbeState::ready, 0.0, 0.0, 100),
      probe("eth0", RouteMedium::wired, RouteProbeState::ready, kGigabit, kGigabit, 900),
  };
  expect(select_route(RoutePolicy{}, probes, true) == 1, "a measured route beats an unmeasured one");
  const std::vector only_unmeasured{
      probe("tb0", RouteMedium::wired, RouteProbeState::ready, 0.0, 0.0, 100),
  };
  expect(select_route(RoutePolicy{}, only_unmeasured, true) == 0,
         "an authenticated but unmeasured route is still usable");
}

void test_excluded_medium_never_chosen() {
  RoutePolicy wired_only;
  wired_only.tiers = {{RouteMedium::loopback}, {RouteMedium::wired}};
  const std::vector probes{
      probe("wifi0", RouteMedium::wifi, RouteProbeState::ready, kFast, kFast, 100),
  };
  expect(!select_route(wired_only, probes, true), "excluded medium is not a route");
  expect(!route_tier(wired_only, RouteMedium::wifi), "wifi has no tier");
}

void test_failures_are_not_routes() {
  const std::vector probes{
      probe("eth0", RouteMedium::wired, RouteProbeState::failed),
      probe("wifi0", RouteMedium::wifi, RouteProbeState::failed),
  };
  expect(!select_route(RoutePolicy{}, probes, true), "nothing answered");
}

void expect_invalid(const RoutePolicy& policy, std::string_view what) {
  bool threw = false;
  try {
    validate_route_policy(policy);
  } catch (const RemoteTransportError& error) {
    threw = error.code() == RemoteErrorCode::invalid_configuration;
  }
  expect(threw, what);
}

void test_policy_validation() {
  RoutePolicy duplicate;
  duplicate.tiers = {{RouteMedium::wired}, {RouteMedium::wired}};
  expect_invalid(duplicate, "a medium in two tiers is rejected");
  RoutePolicy oversize;
  oversize.probe_bytes = kMaxRouteProbeBytes + 1;
  expect_invalid(oversize, "a probe larger than workers answer is rejected");
  validate_route_policy(RoutePolicy{});
}

}  // namespace

int main() {
  return run_tests("svp-exec-route-policy-tests",
                   {
                       {"wired beats faster wifi", test_wired_beats_faster_wifi},
                       {"wifi waits for pending wired", test_wifi_waits_for_pending_wired},
                       {"slower direction decides", test_slower_direction_decides},
                       {"pending same tier is awaited", test_pending_same_tier_is_awaited},
                       {"hopeless pending probe is not awaited",
                        test_hopeless_pending_probe_is_not_awaited},
                       {"worse tier pending does not delay", test_worse_tier_pending_does_not_delay},
                       {"latency breaks throughput ties", test_latency_breaks_throughput_ties},
                       {"unmeasured ranks last in tier", test_unmeasured_ranks_last_in_tier},
                       {"excluded medium never chosen", test_excluded_medium_never_chosen},
                       {"failures are not routes", test_failures_are_not_routes},
                       {"policy validation", test_policy_validation},
                   });
}
