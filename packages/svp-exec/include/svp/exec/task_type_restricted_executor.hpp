#pragma once

#include "svp/exec/executor.hpp"

#include <set>
#include <string>
#include <string_view>

namespace svp::exec {

// An executor that accepts only some task types, wrapping another executor
// (plan §3.1: the coordinator chooses which task types may run where; for
// example frame-batch tasks on remote workers while whole-stage tasks stay in
// the coordinator's process). Every call is forwarded to `inner`; only
// accepts() is narrowed. `inner` must outlive this object, and is started and
// stopped through it.
class TaskTypeRestrictedExecutor final : public Executor {
 public:
  // Throws ExecError(invalid_value) for an empty set of task types.
  TaskTypeRestrictedExecutor(Executor& inner, std::set<std::string, std::less<>> task_types);

  [[nodiscard]] std::string_view id() const override;
  [[nodiscard]] std::size_t slots() const override;
  [[nodiscard]] LossQuarantine loss_quarantine() const override;
  // True when `inner` accepts `spec` and its task type is one of the set.
  [[nodiscard]] bool accepts(const TaskSpec& spec) const override;
  void start(ExecutorEvents& events) override;
  void assign(const TaskSpec& spec, const Lease& lease) override;
  void cancel(std::string_view lease_id) override;
  void lease_expired(std::string_view lease_id) override;
  void stop() override;

 private:
  Executor& inner_;
  std::set<std::string, std::less<>> task_types_;
};

// An executor that accepts every task type except some (the complement of
// TaskTypeRestrictedExecutor), wrapping another executor the same way.
class TaskTypeExcludingExecutor final : public Executor {
 public:
  TaskTypeExcludingExecutor(Executor& inner, std::set<std::string, std::less<>> excluded);

  [[nodiscard]] std::string_view id() const override;
  [[nodiscard]] std::size_t slots() const override;
  [[nodiscard]] LossQuarantine loss_quarantine() const override;
  [[nodiscard]] bool accepts(const TaskSpec& spec) const override;
  void start(ExecutorEvents& events) override;
  void assign(const TaskSpec& spec, const Lease& lease) override;
  void cancel(std::string_view lease_id) override;
  void lease_expired(std::string_view lease_id) override;
  void stop() override;

 private:
  Executor& inner_;
  std::set<std::string, std::less<>> excluded_;
};

}  // namespace svp::exec
