#pragma once

// An in-process executor whose running tasks are reported, by type, to the
// build's LocalTaskLoad (distributed_execution.hpp, M6), so the worker agent
// on this Mac leaves those slots to this build. Every call is forwarded to
// `inner`; a lease counts from assign() until its attempt is reported
// (finished, failed, rejected) or the scheduler gives it up (cancel, expiry,
// stop), as the scheduler counts the slot. `inner` must outlive this object.

#include "svp/builder/distributed_execution.hpp"
#include "svp/exec/executor.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace svp::builder::engine {

class LoadReportingExecutor final : public svp::exec::Executor, svp::exec::ExecutorEvents {
 public:
  LoadReportingExecutor(svp::exec::Executor& inner, std::shared_ptr<LocalTaskLoad> load);
  ~LoadReportingExecutor() override;

  [[nodiscard]] std::string_view id() const override;
  [[nodiscard]] std::size_t slots() const override;
  [[nodiscard]] svp::exec::LossQuarantine loss_quarantine() const override;
  [[nodiscard]] bool accepts(const svp::exec::TaskSpec& spec) const override;
  void start(svp::exec::ExecutorEvents& events) override;
  void assign(const svp::exec::TaskSpec& spec, const svp::exec::Lease& lease) override;
  void cancel(std::string_view lease_id) override;
  void lease_expired(std::string_view lease_id) override;
  void stop() override;

 private:
  void lease_heartbeat(std::string_view lease_id) override;
  void attempt_finished(std::string_view lease_id, svp::exec::AttemptOutput output) override;
  void attempt_failed(std::string_view lease_id, svp::exec::AttemptFailureKind kind,
                      std::string message) override;
  void attempt_rejected(std::string_view lease_id, std::string code,
                        std::string message) override;
  void end(std::string_view lease_id);

  svp::exec::Executor& inner_;
  std::shared_ptr<LocalTaskLoad> load_;
  svp::exec::ExecutorEvents* events_ = nullptr;
  std::mutex mutex_;
  // lease_id -> task type, for leases still counted.
  std::map<std::string, std::string, std::less<>> running_;
};

}  // namespace svp::builder::engine
