#pragma once

// The visual tracking stage's progress while it runs as window tasks
// (tracking_window_plan.hpp), wherever the windows run:
//   * the stage starts when the first window is leased (or, when every
//     window was restored by --resume, when the fold starts);
//   * it advances by one window per committed window;
//   * it completes when the fold has written the stage's artifacts, so the
//     stage's span covers the windows and the fold;
//   * one stderr line per lost or failed window attempt, so a worker dropping
//     out is visible while the build goes on;
//   * per-executor totals for the end-of-build summary, with the leases each
//     executor declined (declined_lease_tally.hpp).
// observe() is called on the scheduler thread; the stage calls are made by
// the fold's task on an executor thread. Thread-safe.

#include "engine/declined_lease_tally.hpp"
#include "engine/tracking_window_plan.hpp"

#include "svp/builder/build_progress.hpp"
#include "svp/exec/attempt_event.hpp"
#include "svp/exec/clock.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace svp::builder::engine {

struct TrackingExecutorTotals {
  std::uint64_t windows = 0;
  std::uint64_t failed_attempts = 0;
  // Sum over committed attempts of lease-to-commit time.
  std::chrono::milliseconds busy{0};
};

class TrackingWindowProgress {
 public:
  TrackingWindowProgress(const TrackingWindowPlan& windows, BuildProgressSink& sink,
                         const svp::exec::Clock& clock, bool quiet,
                         const std::set<std::string>& resumed_task_ids);

  void observe(const svp::exec::AttemptEvent& event);

  // The fold is starting / has finished.
  void fold_started();
  void fold_finished();

  // One line per executor that ran windows, then one per executor that
  // declined any, for stderr.
  [[nodiscard]] std::string summary() const;

 private:
  void start_locked();

  BuildProgressSink& sink_;
  const svp::exec::Clock& clock_;
  bool quiet_;
  mutable std::mutex mutex_;
  std::set<std::string> pending_;
  std::uint64_t total_windows_ = 0;
  std::uint64_t done_windows_ = 0;
  bool started_ = false;
  bool completed_ = false;
  std::map<std::string, std::chrono::milliseconds> leased_at_;
  std::map<std::string, TrackingExecutorTotals> totals_;
  DeclinedLeaseTally declined_;
};

}  // namespace svp::builder::engine
