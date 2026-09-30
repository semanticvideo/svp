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
  if (policy.attempt_deadline_floor < policy.lease_floor ||
      policy.attempt_deadline_factor < policy.lease_factor) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "lease policy attempt deadline must not be shorter than the lease: "
                    "attempt_deadline_floor >= lease_floor and "
                    "attempt_deadline_factor >= lease_factor");
  }
}

namespace {

// max(floor, factor × estimated_seconds) in milliseconds, saturating.
std::chrono::milliseconds scaled_with_floor(std::chrono::milliseconds floor,
                                            std::uint64_t factor,
                                            std::uint64_t estimated_seconds) {
  using Rep = std::chrono::milliseconds::rep;
  constexpr auto kMaxMs = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
  constexpr std::uint64_t kMsPerSecond = 1000;
  std::uint64_t scaled_ms = kMaxMs;
  if (estimated_seconds <= kMaxMs / kMsPerSecond / factor) {
    scaled_ms = estimated_seconds * kMsPerSecond * factor;
  }
  const auto floor_ms = static_cast<std::uint64_t>(floor.count());
  return std::chrono::milliseconds(
      static_cast<Rep>(scaled_ms > floor_ms ? scaled_ms : floor_ms));
}

}  // namespace

std::chrono::milliseconds lease_duration(const LeasePolicy& policy,
                                         std::uint64_t estimated_seconds) {
  return scaled_with_floor(policy.lease_floor, policy.lease_factor, estimated_seconds);
}

std::chrono::milliseconds attempt_deadline(const LeasePolicy& policy,
                                           std::uint64_t estimated_seconds) {
  return scaled_with_floor(policy.attempt_deadline_floor, policy.attempt_deadline_factor,
                           estimated_seconds);
}

}  // namespace svp::exec
