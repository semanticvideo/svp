#pragma once

#include "svp/exec/lease_policy.hpp"
#include "svp/exec/retry_policy.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

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

// Lease and retry rules for one task type, replacing the policy-wide ones
// for tasks of that type. Builds mix task types whose failure behaviour
// differs: a whole pipeline stage in the coordinator's process runs once with
// a lease far beyond any stage, while a small frame-batch task on a worker
// needs a short lease (to notice a lost worker) and retries elsewhere.
struct TaskTypePolicy {
  LeasePolicy lease;
  // RetryPolicy::max_attempts for tasks of this type. Executor quarantine
  // (RetryPolicy::quarantine_after_loss_events, prefer_different_executor)
  // stays policy-wide: it is about executors, not task types.
  std::uint64_t max_attempts = kDefaultMaxAttempts;
};

struct SchedulerPolicy {
  LeasePolicy lease;
  RetryPolicy retry;
  // Per task type (TaskSpec::task_type) overrides of `lease` and
  // retry.max_attempts; types not listed use the policy-wide values.
  std::map<std::string, TaskTypePolicy, std::less<>> task_types;
  // Plan §4.4 stragglers: when no ready work remains, an idle executor may run
  // a duplicate of the longest-running lease; the first verified result
  // commits and the other is a determinism check. Off by default until
  // measured (plan milestone M4).
  bool speculative_duplicates = false;
  std::chrono::milliseconds max_idle_wait = kDefaultMaxIdleWait;
  std::chrono::milliseconds rejection_backoff = kDefaultRejectionBackoff;
};

// Validates the nested policies (every task-type override included); throws
// ExecError(invalid_value) when max_idle_wait or rejection_backoff is not
// positive.
void validate_scheduler_policy(const SchedulerPolicy& policy);

// The lease policy and attempt limit tasks of `task_type` run under.
[[nodiscard]] const LeasePolicy& lease_policy_for(const SchedulerPolicy& policy,
                                                  std::string_view task_type);
[[nodiscard]] std::uint64_t max_attempts_for(const SchedulerPolicy& policy,
                                             std::string_view task_type);

}  // namespace svp::exec
