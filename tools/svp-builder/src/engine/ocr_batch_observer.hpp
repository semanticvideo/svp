#pragma once

// Watches the scheduler's attempt events for a build's OCR frame-batch tasks
// (wherever they run) and turns them into what a person and the run report
// see:
//   * OCR stage progress: the stage starts when the first batch is leased,
//     advances by each committed batch's samples, and completes when every
//     batch has committed (batches restored by --resume count as done);
//   * one stderr line per lost or failed batch attempt and per quarantined
//     executor, so a worker dropping out is visible while the build goes on;
//   * per-executor totals for the end-of-build summary.
// Called on the scheduler thread only (svp::exec::AttemptObserver).

#include "engine/ocr_frame_batch_plan.hpp"

#include "svp/builder/build_progress.hpp"
#include "svp/exec/attempt_event.hpp"
#include "svp/exec/clock.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svp::builder::engine {

struct OcrExecutorTotals {
  std::uint64_t batches = 0;
  std::uint64_t samples = 0;
  std::uint64_t failed_attempts = 0;
  // Sum over committed attempts of lease-to-commit time.
  std::chrono::milliseconds busy{0};
};

class OcrBatchObserver {
 public:
  // `on_all_committed`, when set, runs once (on the scheduler thread) when
  // the last batch commits: nothing will lease a batch again.
  OcrBatchObserver(const OcrFrameBatchPlan& batches, BuildProgressSink& sink,
                   const svp::exec::Clock& clock, bool quiet,
                   const std::set<std::string>& resumed_task_ids,
                   std::function<void()> on_all_committed = {});

  void observe(const svp::exec::AttemptEvent& event);

  [[nodiscard]] const std::map<std::string, OcrExecutorTotals>& totals() const {
    return totals_;
  }
  [[nodiscard]] std::uint64_t retried_attempts() const { return retried_attempts_; }
  [[nodiscard]] const std::vector<std::string>& quarantined() const { return quarantined_; }

  // One line per executor that ran batches, for stderr.
  [[nodiscard]] std::string summary() const;

 private:
  BuildProgressSink& sink_;
  std::function<void()> on_all_committed_;
  const svp::exec::Clock& clock_;
  bool quiet_;
  std::map<std::string, std::uint64_t> samples_by_task_;
  std::uint64_t total_samples_ = 0;
  std::uint64_t done_samples_ = 0;
  std::size_t pending_batches_ = 0;
  bool started_ = false;
  bool completed_ = false;
  std::map<std::string, std::chrono::milliseconds> leased_at_;
  std::map<std::string, OcrExecutorTotals> totals_;
  std::uint64_t retried_attempts_ = 0;
  std::vector<std::string> quarantined_;
};

}  // namespace svp::builder::engine
