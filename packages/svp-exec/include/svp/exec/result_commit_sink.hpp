#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// One verified, committed result: the TaskResult, its output bytes in
// output order, and the executor that produced it.
struct CommittedResult {
  TaskResult result;
  std::vector<FramePayload> payloads;
  std::string executor_id;
};

// Where the scheduler hands each verified result, exactly once per task
// (plan §4.4 "committed (journal)"). The recovery journal (RC2 §20.4) is the
// production sink. commit() is called on the scheduler thread only; an
// exception from it fails the build and nothing else is committed.
class ResultCommitSink {
 public:
  virtual ~ResultCommitSink() = default;
  virtual void commit(const TaskSpec& spec, const CommittedResult& committed) = 0;
};

// Keeps every committed result in memory, keyed by task_id. For tests and
// for callers that reduce in memory.
class InMemoryResultCommitSink final : public ResultCommitSink {
 public:
  // Throws ExecError(invalid_value) if the task was already committed.
  void commit(const TaskSpec& spec, const CommittedResult& committed) override;

  [[nodiscard]] const CommittedResult* find(std::string_view task_id) const;
  [[nodiscard]] std::vector<CommittedResult> results() const;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  std::map<std::string, CommittedResult, std::less<>> results_;
};

}  // namespace svp::exec
