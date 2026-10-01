#include "svp/exec/task_type_restricted_executor.hpp"

#include "svp/exec/exec_error.hpp"

#include <utility>

namespace svp::exec {

TaskTypeRestrictedExecutor::TaskTypeRestrictedExecutor(
    Executor& inner, std::set<std::string, std::less<>> task_types)
    : inner_(inner), task_types_(std::move(task_types)) {
  if (task_types_.empty()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "a task-type restricted executor needs at least one task type");
  }
}

std::string_view TaskTypeRestrictedExecutor::id() const { return inner_.id(); }
std::size_t TaskTypeRestrictedExecutor::slots() const { return inner_.slots(); }
LossQuarantine TaskTypeRestrictedExecutor::loss_quarantine() const {
  return inner_.loss_quarantine();
}
bool TaskTypeRestrictedExecutor::accepts(const TaskSpec& spec) const {
  return task_types_.contains(spec.task_type) && inner_.accepts(spec);
}
void TaskTypeRestrictedExecutor::start(ExecutorEvents& events) { inner_.start(events); }
void TaskTypeRestrictedExecutor::assign(const TaskSpec& spec, const Lease& lease) {
  inner_.assign(spec, lease);
}
void TaskTypeRestrictedExecutor::cancel(std::string_view lease_id) { inner_.cancel(lease_id); }
void TaskTypeRestrictedExecutor::lease_expired(std::string_view lease_id) {
  inner_.lease_expired(lease_id);
}
void TaskTypeRestrictedExecutor::stop() { inner_.stop(); }

TaskTypeExcludingExecutor::TaskTypeExcludingExecutor(Executor& inner,
                                                     std::set<std::string, std::less<>> excluded)
    : inner_(inner), excluded_(std::move(excluded)) {}

std::string_view TaskTypeExcludingExecutor::id() const { return inner_.id(); }
std::size_t TaskTypeExcludingExecutor::slots() const { return inner_.slots(); }
LossQuarantine TaskTypeExcludingExecutor::loss_quarantine() const {
  return inner_.loss_quarantine();
}
bool TaskTypeExcludingExecutor::accepts(const TaskSpec& spec) const {
  return !excluded_.contains(spec.task_type) && inner_.accepts(spec);
}
void TaskTypeExcludingExecutor::start(ExecutorEvents& events) { inner_.start(events); }
void TaskTypeExcludingExecutor::assign(const TaskSpec& spec, const Lease& lease) {
  inner_.assign(spec, lease);
}
void TaskTypeExcludingExecutor::cancel(std::string_view lease_id) { inner_.cancel(lease_id); }
void TaskTypeExcludingExecutor::lease_expired(std::string_view lease_id) {
  inner_.lease_expired(lease_id);
}
void TaskTypeExcludingExecutor::stop() { inner_.stop(); }

}  // namespace svp::exec
