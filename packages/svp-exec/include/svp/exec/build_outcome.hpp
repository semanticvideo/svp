#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/result_commit_sink.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

enum class BuildStatus {
  // Every task committed exactly one verified result (in this run or, on
  // resume, in the run it continues).
  succeeded,
  failed,
  // The cancellation token was set; committed results stay committed.
  cancelled,
};

enum class BuildFailureKind {
  // A task reached RetryPolicy.max_attempts failed attempts (plan §7.3).
  retries_exhausted,
  // A task returned a failed result marked not retryable.
  permanent_task_failure,
  // Two verified results for one task disagree (plan §4.4 duplicates).
  determinism_incident,
  // Work remains but every executor is quarantined.
  no_usable_executor,
  // The ResultCommitSink threw.
  commit_failed,
};

[[nodiscard]] std::string_view build_status_name(BuildStatus status) noexcept;
[[nodiscard]] std::string_view build_failure_kind_name(BuildFailureKind kind) noexcept;

struct BuildFailure {
  BuildFailureKind kind = BuildFailureKind::retries_exhausted;
  // The task that caused the failure; empty for no_usable_executor.
  std::string task_id;
  std::string message;
};

// Two verified results for one task with different output digests. Plan
// §7.3: both results are kept; the committed one is in the sink, the
// conflicting one is here.
struct DeterminismIncident {
  std::string task_id;
  std::uint64_t committed_attempt = 0;
  std::string committed_executor_id;
  Blake3Digest committed_output_digest{};
  CommittedResult conflicting;
};

struct SchedulerStats {
  // Results committed during this run (sink.commit calls).
  std::uint64_t committed = 0;
  // Results committed by an earlier, interrupted run and passed in on resume;
  // their tasks were not run again.
  std::uint64_t resumed = 0;
  std::uint64_t attempts_started = 0;
  std::uint64_t speculative_attempts = 0;
  // Failed attempts that were followed by another attempt.
  std::uint64_t retries = 0;
  std::uint64_t leases_expired = 0;
  // Attempts cancelled at their hard deadline (LeasePolicy).
  std::uint64_t deadlines_exceeded = 0;
  std::uint64_t invalid_results = 0;
  // Later verified results whose digest equals the committed one.
  std::uint64_t duplicates_discarded = 0;
  std::vector<std::string> quarantined_executors;
};

struct BuildOutcome {
  BuildStatus status = BuildStatus::failed;
  std::optional<BuildFailure> failure;
  std::vector<DeterminismIncident> incidents;
  SchedulerStats stats;
};

}  // namespace svp::exec
