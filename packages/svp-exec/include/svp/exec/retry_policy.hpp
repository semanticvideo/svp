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
//   * quarantine_after_loss_events 3: plan §4.4 quarantines "a worker with
//     repeated failures". The count is of loss EVENTS, not lost attempts,
//     and only of consecutive ones (scheduler.hpp, loss quarantine):
//       - one event: every attempt already outstanding on the executor when
//         a loss is counted is presumed a casualty of the same cause (a
//         dropped session or crashed worker loses all its in-flight leases
//         at once). Only the loss of an attempt granted after the latest
//         counted event is a new, independent event. So one crash of a
//         worker with any number of slots is one event, whatever the slot
//         count of the Mac it runs on.
//       - consecutive: a verified result from an attempt granted after the
//         latest event proves the executor recovered (for example it
//         reconnected) and resets the count to zero.
//     A single crash or sleep does not remove a worker; a worker that keeps
//     losing work with no successful attempt in between does. An invalid
//     result quarantines immediately, independent of this count.
inline constexpr std::uint64_t kDefaultMaxAttempts = 3;
inline constexpr std::uint64_t kDefaultQuarantineAfterLossEvents = 3;

struct RetryPolicy {
  // Failed attempts (retryable task failure, lease expiry, executor loss,
  // invalid result) a task may accumulate. The build fails when a task
  // reaches it with no attempt still running. A speculative duplicate is not
  // a retry and only counts if it fails.
  std::uint64_t max_attempts = kDefaultMaxAttempts;
  bool prefer_different_executor = true;
  // Consecutive loss events (see above; an event is one or more lost,
  // expired, or deadline-exceeded attempts) after which an executor whose
  // loss_quarantine() is after_repeated_losses is quarantined.
  std::uint64_t quarantine_after_loss_events = kDefaultQuarantineAfterLossEvents;
};

// Throws ExecError(invalid_value) when max_attempts or
// quarantine_after_loss_events is 0.
void validate_retry_policy(const RetryPolicy& policy);

}  // namespace svp::exec
