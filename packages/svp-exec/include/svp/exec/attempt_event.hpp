#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace svp::exec {

// Scheduler lifecycle transitions (plan §4.4), reported for run reports,
// progress, and tests.
enum class AttemptEventKind {
  leased,
  committed,
  duplicate_discarded,
  determinism_incident,
  // The attempt failed (task failure, invalid result, executor lost).
  failed,
  // The executor declined the lease before running it (admission); the task
  // was offered elsewhere and nothing was counted against it.
  rejected,
  expired,
  // The attempt ran past its hard deadline (LeasePolicy) while its lease was
  // still renewed; it was cancelled and counted as lost.
  deadline_exceeded,
  // The executor stopped receiving work for the rest of the run.
  executor_quarantined,
};

[[nodiscard]] std::string_view attempt_event_kind_name(AttemptEventKind kind) noexcept;

struct AttemptEvent {
  AttemptEventKind kind = AttemptEventKind::leased;
  // Empty for executor_quarantined.
  std::string task_id;
  std::uint64_t attempt = 0;
  std::string executor_id;
  std::string lease_id;
  bool speculative = false;
  std::string detail;
};

// Called on the scheduler thread; keep it quick and do not call back into
// the scheduler.
using AttemptObserver = std::function<void(const AttemptEvent&)>;

}  // namespace svp::exec
