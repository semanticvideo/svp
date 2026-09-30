#pragma once

#include <cstdint>

namespace svp::exec {

// Plan §4.4 retries and quarantine.
//
// Defaults and why:
//   * max_attempts 3: one transient loss (a worker crash or lease expiry),
//     one more try on a different executor, and a third try before
//     concluding the task or the fleet is broken. Plan §7.3: retries
//     exhausted fails the build; a gap is never silently accepted.
//   * prefer_different_executor: a retry goes to an executor that has not
//     already failed this task whenever one is usable (plan §4.4 "a retry
//     prefers a different worker"); it still runs on a previously failing
//     executor when no other is usable.
//   * quarantine_after_executor_failures 3: plan §4.4 quarantines "a worker
//     with repeated failures". A single crash or sleep does not remove a
//     worker, a worker that keeps losing work does. An invalid result
//     quarantines immediately, independent of this count.
inline constexpr std::uint64_t kDefaultMaxAttempts = 3;
inline constexpr std::uint64_t kDefaultQuarantineAfterExecutorFailures = 3;

struct RetryPolicy {
  // Failed attempts (retryable task failure, lease expiry, executor loss,
  // invalid result) a task may accumulate. The build fails when a task
  // reaches it with no attempt still running. A speculative duplicate is not
  // a retry and only counts if it fails.
  std::uint64_t max_attempts = kDefaultMaxAttempts;
  bool prefer_different_executor = true;
  // Lost or expired attempts after which an executor is quarantined.
  std::uint64_t quarantine_after_executor_failures =
      kDefaultQuarantineAfterExecutorFailures;
};

// Throws ExecError(invalid_value) when max_attempts or
// quarantine_after_executor_failures is 0.
void validate_retry_policy(const RetryPolicy& policy);

}  // namespace svp::exec
