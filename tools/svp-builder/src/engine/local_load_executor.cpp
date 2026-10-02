#include "local_load_executor.hpp"

#include <utility>

namespace svp::builder::engine {

LoadReportingExecutor::LoadReportingExecutor(svp::exec::Executor& inner,
                                             std::shared_ptr<LocalTaskLoad> load)
    : inner_(inner), load_(std::move(load)) {}

LoadReportingExecutor::~LoadReportingExecutor() {
  const std::lock_guard lock(mutex_);
  for (const auto& [lease_id, task_type] : running_) {
    load_->finished(task_type);
  }
  running_.clear();
}

std::string_view LoadReportingExecutor::id() const { return inner_.id(); }
std::size_t LoadReportingExecutor::slots() const { return inner_.slots(); }
svp::exec::LossQuarantine LoadReportingExecutor::loss_quarantine() const {
  return inner_.loss_quarantine();
}
bool LoadReportingExecutor::accepts(const svp::exec::TaskSpec& spec) const {
  return inner_.accepts(spec);
}

void LoadReportingExecutor::start(svp::exec::ExecutorEvents& events) {
  events_ = &events;
  inner_.start(*this);
}

void LoadReportingExecutor::assign(const svp::exec::TaskSpec& spec,
                                   const svp::exec::Lease& lease) {
  {
    const std::lock_guard lock(mutex_);
    running_[lease.lease_id] = spec.task_type;
  }
  load_->started(spec.task_type);
  inner_.assign(spec, lease);
}

void LoadReportingExecutor::end(std::string_view lease_id) {
  std::string task_type;
  {
    const std::lock_guard lock(mutex_);
    const auto found = running_.find(lease_id);
    if (found == running_.end()) {
      return;
    }
    task_type = std::move(found->second);
    running_.erase(found);
  }
  load_->finished(task_type);
}

void LoadReportingExecutor::cancel(std::string_view lease_id) {
  end(lease_id);
  inner_.cancel(lease_id);
}

void LoadReportingExecutor::lease_expired(std::string_view lease_id) {
  end(lease_id);
  inner_.lease_expired(lease_id);
}

void LoadReportingExecutor::stop() {
  inner_.stop();
  std::map<std::string, std::string, std::less<>> running;
  {
    const std::lock_guard lock(mutex_);
    running.swap(running_);
  }
  for (const auto& [lease_id, task_type] : running) {
    load_->finished(task_type);
  }
}

void LoadReportingExecutor::lease_heartbeat(std::string_view lease_id) {
  events_->lease_heartbeat(lease_id);
}

void LoadReportingExecutor::attempt_finished(std::string_view lease_id,
                                             svp::exec::AttemptOutput output) {
  end(lease_id);
  events_->attempt_finished(lease_id, std::move(output));
}

void LoadReportingExecutor::attempt_failed(std::string_view lease_id,
                                           svp::exec::AttemptFailureKind kind,
                                           std::string message) {
  end(lease_id);
  events_->attempt_failed(lease_id, kind, std::move(message));
}

void LoadReportingExecutor::attempt_rejected(std::string_view lease_id, std::string code,
                                             std::string message) {
  end(lease_id);
  events_->attempt_rejected(lease_id, std::move(code), std::move(message));
}

}  // namespace svp::builder::engine
