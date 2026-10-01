#pragma once

// Scheduler policy for whole-stage tasks run by the in-process executor.
//
// The svp-exec defaults are tuned for small tasks on remote workers: leases
// that expire when a worker goes quiet, a hard deadline scaled from an
// estimate, and retries on another executor. None of that fits a whole stage
// of today's pipeline running in the coordinator's own process:
//   * a stage's duration grows with the media (an OCR stage over a two-hour
//     webinar runs for hours) and is not known at plan time, so a deadline
//     would cancel healthy work; the lease and deadline are set to
//     kWholeStageAttemptBound, far beyond any stage, and Ctrl-C stays the way
//     to stop a build;
//   * a failed stage leaves partial files in the shared staging directory and
//     stage failures are deterministic, so a stage runs once
//     (max_attempts 1) and its failure fails the build, as it always has.

#include "svp/exec/scheduler_policy.hpp"

#include <chrono>

namespace svp::builder::engine {

// 30 days: longer than any single stage of any build, and small enough that
// steady-clock arithmetic on it cannot overflow.
inline constexpr std::chrono::milliseconds kWholeStageAttemptBound{
    std::chrono::hours(24 * 30)};

[[nodiscard]] svp::exec::SchedulerPolicy whole_stage_scheduler_policy();

}  // namespace svp::builder::engine
