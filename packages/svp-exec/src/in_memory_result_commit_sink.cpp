#include "svp/exec/exec_error.hpp"
#include "svp/exec/result_commit_sink.hpp"

namespace svp::exec {

void InMemoryResultCommitSink::commit(const TaskSpec& spec,
                                      const CommittedResult& committed) {
  if (!results_.emplace(spec.task_id, committed).second) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task `" + spec.task_id + "` was committed twice");
  }
}

const CommittedResult* InMemoryResultCommitSink::find(std::string_view task_id) const {
  const auto found = results_.find(task_id);
  return found == results_.end() ? nullptr : &found->second;
}

std::vector<CommittedResult> InMemoryResultCommitSink::results() const {
  std::vector<CommittedResult> all;
  all.reserve(results_.size());
  for (const auto& [task_id, committed] : results_) {
    all.push_back(committed);
  }
  return all;
}

std::size_t InMemoryResultCommitSink::size() const noexcept { return results_.size(); }

}  // namespace svp::exec
