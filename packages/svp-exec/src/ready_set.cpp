#include "svp/exec/task_graph.hpp"

#include <algorithm>

namespace svp::exec {

ReadySet::ReadySet(const TaskGraph& graph)
    : graph_(&graph),
      pending_dependencies_(graph.size()),
      complete_(graph.size(), false) {
  for (std::size_t index = 0; index < graph.size(); ++index) {
    pending_dependencies_[index] = graph.dependencies(index).size();
  }
}

std::vector<std::size_t> ReadySet::initially_ready() const {
  std::vector<std::size_t> ready;
  for (std::size_t index = 0; index < graph_->size(); ++index) {
    if (graph_->dependencies(index).empty()) {
      ready.push_back(index);
    }
  }
  return ready;
}

std::vector<std::size_t> ReadySet::complete(std::size_t index) {
  if (complete_.at(index)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task `" + graph_->node(index).spec.task_id +
                        "` was completed twice");
  }
  complete_[index] = true;
  ++completed_count_;
  std::vector<std::size_t> ready;
  for (const std::size_t dependent : graph_->dependents(index)) {
    if (--pending_dependencies_[dependent] == 0) {
      ready.push_back(dependent);
    }
  }
  std::sort(ready.begin(), ready.end());
  return ready;
}

bool ReadySet::is_complete(std::size_t index) const { return complete_.at(index); }

bool ReadySet::all_complete() const noexcept {
  return completed_count_ == graph_->size();
}

}  // namespace svp::exec
