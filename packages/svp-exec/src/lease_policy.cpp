#include "svp/exec/lease_policy.hpp"

#include "svp/exec/exec_error.hpp"

#include <limits>

namespace svp::exec {

void validate_lease_policy(const LeasePolicy& policy) {
  if (policy.lease_floor.count() <= 0 || policy.heartbeat_interval.count() <= 0 ||
      policy.lease_factor == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "lease policy needs a positive lease_floor and "
                    "heartbeat_interval and a lease_factor >= 1");
  }
  if (policy.heartbeat_interval.count() *
          static_cast<std::chrono::milliseconds::rep>(kMinHeartbeatsPerLeaseFloor) >=
      policy.lease_floor.count()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "lease policy heartbeat_interval must be less than "
                    "lease_floor / 3 (plan §4.4)");
  }
}

std::chrono::milliseconds lease_duration(const LeasePolicy& policy,
                                         std::uint64_t estimated_seconds) {
  using Rep = std::chrono::milliseconds::rep;
  constexpr auto kMaxMs = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
  constexpr std::uint64_t kMsPerSecond = 1000;
  std::uint64_t scaled_ms = kMaxMs;
  if (estimated_seconds <= kMaxMs / kMsPerSecond / policy.lease_factor) {
    scaled_ms = estimated_seconds * kMsPerSecond * policy.lease_factor;
  }
  const auto floor_ms = static_cast<std::uint64_t>(policy.lease_floor.count());
  return std::chrono::milliseconds(
      static_cast<Rep>(scaled_ms > floor_ms ? scaled_ms : floor_ms));
}

}  // namespace svp::exec
