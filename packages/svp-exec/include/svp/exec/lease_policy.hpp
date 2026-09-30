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

// Hard per-attempt deadline. Heartbeats prove that the process running an
// attempt is alive, not that the task is making progress: a task function
// stuck in a loop or a wedged decoder keeps a live worker heartbeating forever.
// So every attempt also gets
//   attempt_deadline = max(attempt_deadline_floor,
//                          attempt_deadline_factor × estimated_duration)
// measured from its grant and never renewed. When it passes, the scheduler
// cancels the attempt (its CancellationToken is set) and treats it as lost:
// the task is retried under RetryPolicy and the executor's loss count grows
// (see LossQuarantine).
//
// Defaults and why:
//   * attempt_deadline_factor 8: estimates are per-type averages (plan §4.4),
//     and plan §2.2 measured the slowest OCR frame at 3.2x the mean (10.4 s vs
//     3.2 s). 8x leaves 2.5x headroom above that spread, and is 4x
//     lease_factor, so a slow attempt that still heartbeats is never cut off
//     for being slow.
//   * attempt_deadline_floor 10 min: the longest single unit measured in plan
//     §2.2 is a tracking window of about 49 s; 10 min is over 12x that, so a
//     task whose estimate is low or not yet measured is not cut off, while a
//     stuck attempt holds its slot for at most minutes, not for the build.
// Validation keeps the deadline at or beyond the lease for every estimate
// (floor >= lease_floor, factor >= lease_factor), so it only ever fires on an
// attempt whose lease heartbeats kept alive.
inline constexpr std::chrono::milliseconds kDefaultAttemptDeadlineFloor{600'000};
inline constexpr std::uint64_t kDefaultAttemptDeadlineFactor = 8;

struct LeasePolicy {
  std::chrono::milliseconds lease_floor = kDefaultLeaseFloor;
  std::uint64_t lease_factor = kDefaultLeaseFactor;
  std::chrono::milliseconds heartbeat_interval = kDefaultHeartbeatInterval;
  std::chrono::milliseconds attempt_deadline_floor = kDefaultAttemptDeadlineFloor;
  std::uint64_t attempt_deadline_factor = kDefaultAttemptDeadlineFactor;
};

// Throws ExecError(invalid_value) unless lease_floor and heartbeat_interval
// are positive, lease_factor >= 1,
// heartbeat_interval × kMinHeartbeatsPerLeaseFloor < lease_floor,
// attempt_deadline_floor >= lease_floor, and
// attempt_deadline_factor >= lease_factor.
void validate_lease_policy(const LeasePolicy& policy);

// max(lease_floor, lease_factor × estimated_seconds), saturating instead of
// overflowing.
[[nodiscard]] std::chrono::milliseconds lease_duration(
    const LeasePolicy& policy, std::uint64_t estimated_seconds);

// max(attempt_deadline_floor, attempt_deadline_factor × estimated_seconds),
// saturating instead of overflowing.
[[nodiscard]] std::chrono::milliseconds attempt_deadline(
    const LeasePolicy& policy, std::uint64_t estimated_seconds);

}  // namespace svp::exec
