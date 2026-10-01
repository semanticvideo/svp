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

// How long an executor that rejected a lease (admission: insufficient memory,
// memory pressure, no capacity) is offered no new leases, unless it reports
// capacity sooner by finishing an attempt. 2 s: memory on a worker frees as
// its running tasks end, which takes seconds (plan §2.2: OCR frames take
// 0.4-10 s), so re-offering at the scheduler's 50 ms wake-up cadence would
// only produce a stream of rejections; and it is short enough that a worker
// whose pressure eased is used again within a fraction of one task's time.
inline constexpr std::chrono::milliseconds kDefaultRejectionBackoff{2'000};

struct SchedulerPolicy {
  LeasePolicy lease;
  RetryPolicy retry;
  // Plan §4.4 stragglers: when no ready work remains, an idle executor may run
  // a duplicate of the longest-running lease; the first verified result
  // commits and the other is a determinism check. Off by default until
  // measured (plan milestone M4).
  bool speculative_duplicates = false;
  std::chrono::milliseconds max_idle_wait = kDefaultMaxIdleWait;
  std::chrono::milliseconds rejection_backoff = kDefaultRejectionBackoff;
};

// Validates the nested policies; throws ExecError(invalid_value) when
// max_idle_wait or rejection_backoff is not positive.
void validate_scheduler_policy(const SchedulerPolicy& policy);

}  // namespace svp::exec
