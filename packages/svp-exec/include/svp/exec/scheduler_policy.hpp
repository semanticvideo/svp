#pragma once

#include "svp/exec/lease_policy.hpp"
#include "svp/exec/retry_policy.hpp"

#include <chrono>

namespace svp::exec {

// Longest the scheduler blocks with no executor event before it re-reads the
// cancellation token and the clock. It bounds cancellation latency and how
// late an expired lease is noticed; 50 ms is imperceptible to a person
// pressing Ctrl-C and costs about 20 idle wake-ups a second.
inline constexpr std::chrono::milliseconds kDefaultMaxIdleWait{50};

struct SchedulerPolicy {
  LeasePolicy lease;
  RetryPolicy retry;
  // Plan §4.4 stragglers: when no ready work remains, an idle executor may run
  // a duplicate of the longest-running lease; the first verified result
  // commits and the other is a determinism check. Off by default until
  // measured (plan milestone M4).
  bool speculative_duplicates = false;
  std::chrono::milliseconds max_idle_wait = kDefaultMaxIdleWait;
};

// Validates the nested policies; throws ExecError(invalid_value) when
// max_idle_wait is not positive.
void validate_scheduler_policy(const SchedulerPolicy& policy);

}  // namespace svp::exec
