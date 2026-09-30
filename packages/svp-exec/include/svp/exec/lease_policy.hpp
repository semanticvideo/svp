#pragma once

#include <chrono>
#include <cstdint>

namespace svp::exec {

// Plan §4.4 leases. A lease lasts
//   lease_duration = max(lease_floor, lease_factor × estimated_duration)
// from its grant and from every heartbeat that renews it. When it runs out
// the scheduler gives the task to another attempt. The estimate is the
// task's TaskResources.est_seconds (plan §4.2); the per-type moving average of
// observed durations (plan §4.4) will feed the same field later.
//
// Defaults and why:
//   * lease_floor 30 s: a healthy Mac that sleeps a display, roams Wi-Fi, or
//     pauses under memory pressure can go quiet for several seconds; 30 s
//     tolerates about five missed heartbeats before work is given away, and a
//     worker that is really gone costs at most 30 s of a build that runs for
//     minutes.
//   * lease_factor 2: a long task keeps its lease for twice its estimate
//     without a heartbeat, so an estimate that is off by up to 2x (estimates
//     are seeded from a single measured session, plan §2.2) never expires a
//     worker that is merely slow to report.
//   * heartbeat_interval 5 s: the plan requires
//     heartbeat_interval < lease_floor / 3; 5 s gives six chances per floor
//     and a heartbeat is one small frame, so the cost is negligible.
inline constexpr std::chrono::milliseconds kDefaultLeaseFloor{30'000};
inline constexpr std::uint64_t kDefaultLeaseFactor = 2;
inline constexpr std::chrono::milliseconds kDefaultHeartbeatInterval{5'000};

// Plan §4.4: "heartbeat_interval < lease_floor / 3".
inline constexpr std::uint64_t kMinHeartbeatsPerLeaseFloor = 3;

struct LeasePolicy {
  std::chrono::milliseconds lease_floor = kDefaultLeaseFloor;
  std::uint64_t lease_factor = kDefaultLeaseFactor;
  std::chrono::milliseconds heartbeat_interval = kDefaultHeartbeatInterval;
};

// Throws ExecError(invalid_value) unless lease_floor and heartbeat_interval
// are positive, lease_factor >= 1, and
// heartbeat_interval × kMinHeartbeatsPerLeaseFloor < lease_floor.
void validate_lease_policy(const LeasePolicy& policy);

// max(lease_floor, lease_factor × estimated_seconds), saturating instead of
// overflowing.
[[nodiscard]] std::chrono::milliseconds lease_duration(
    const LeasePolicy& policy, std::uint64_t estimated_seconds);

}  // namespace svp::exec
