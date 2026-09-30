#include "svp/exec/task_graph.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace svp::exec {
namespace {

std::string describe_order_key(const TaskOrderKey& key) {
  std::string text = key.lane + "[";
  for (std::size_t index = 0; index < key.ordinals.size(); ++index) {
    text += (index == 0 ? "" : ",") + std::to_string(key.ordinals[index]);
  }
  return text + "]";
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  return left > std::numeric_limits<std::uint64_t>::max() - right
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

void validate_node_identities(const std::vector<TaskNode>& nodes) {
  std::set<TaskOrderKey> order_keys;
  for (const TaskNode& node : nodes) {
    try {
      validate_task_spec(node.spec);
    } catch (const ExecError& error) {
      throw TaskGraphError(TaskGraphIssue::invalid_task_spec,
                           "task `" + node.spec.task_id + "`: " + error.what());
    }
    if (node.spec.build_session_id != nodes.front().spec.build_session_id) {
      throw TaskGraphError(TaskGraphIssue::mixed_build_sessions,
                           "task `" + node.spec.task_id + "` belongs to build session `" +
                               node.spec.build_session_id + "`, not `" +
                               nodes.front().spec.build_session_id + "`");
    }
    if (!order_keys.insert(node.order_key).second) {
      throw TaskGraphError(TaskGraphIssue::duplicate_order_key,
                           "task `" + node.spec.task_id + "` repeats order key " +
                               describe_order_key(node.order_key));
    }
  }
}

}  // namespace

bool operator<(const TaskOrderKey& left, const TaskOrderKey& right) {
  if (left.lane != right.lane) {
    return left.lane < right.lane;
  }
  return std::lexicographical_compare(left.ordinals.begin(), left.ordinals.end(),
                                      right.ordinals.begin(), right.ordinals.end());
}

std::string_view task_graph_issue_name(TaskGraphIssue issue) noexcept {
  switch (issue) {
    case TaskGraphIssue::invalid_task_spec:
      return "invalid_task_spec";
    case TaskGraphIssue::duplicate_task_id:
      return "duplicate_task_id";
    case TaskGraphIssue::missing_dependency:
      return "missing_dependency";
    case TaskGraphIssue::dependency_cycle:
      return "dependency_cycle";
    case TaskGraphIssue::duplicate_order_key:
      return "duplicate_order_key";
    case TaskGraphIssue::mixed_build_sessions:
      return "mixed_build_sessions";
  }
  return "unknown";
}

TaskGraphError::TaskGraphError(TaskGraphIssue issue, std::string message)
    : ExecError(ExecErrorCode::invalid_value,
                "task graph " + std::string(task_graph_issue_name(issue)) + ": " +
                    std::move(message)),
      issue_(issue) {}

TaskGraphIssue TaskGraphError::issue() const noexcept { return issue_; }

TaskGraph::TaskGraph(std::vector<TaskNode> nodes) : nodes_(std::move(nodes)) {
  validate_node_identities(nodes_);

  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    if (!index_by_id_.emplace(nodes_[index].spec.task_id, index).second) {
      throw TaskGraphError(TaskGraphIssue::duplicate_task_id,
                           "task `" + nodes_[index].spec.task_id +
                               "` appears more than once");
    }
  }

  dependencies_.resize(nodes_.size());
  dependents_.resize(nodes_.size());
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    for (const std::string& dependency : nodes_[index].spec.depends_on) {
      const auto found = index_by_id_.find(dependency);
      if (found == index_by_id_.end()) {
        throw TaskGraphError(TaskGraphIssue::missing_dependency,
                             "task `" + nodes_[index].spec.task_id +
                                 "` depends on unknown task `" + dependency + "`");
      }
      dependencies_[index].push_back(found->second);
      dependents_[found->second].push_back(index);
    }
  }

  // Kahn's algorithm; the resulting order also drives the critical-path pass.
  std::vector<std::size_t> pending(nodes_.size());
  std::vector<std::size_t> order;
  order.reserve(nodes_.size());
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    pending[index] = dependencies_[index].size();
    if (pending[index] == 0) {
      order.push_back(index);
    }
  }
  for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
    for (const std::size_t dependent : dependents_[order[cursor]]) {
      if (--pending[dependent] == 0) {
        order.push_back(dependent);
      }
    }
  }
  if (order.size() != nodes_.size()) {
    const auto stuck = std::find_if(pending.begin(), pending.end(),
                                    [](std::size_t count) { return count != 0; });
    throw TaskGraphError(
        TaskGraphIssue::dependency_cycle,
        "task `" + nodes_[static_cast<std::size_t>(stuck - pending.begin())].spec.task_id +
            "` is on or behind a dependency cycle");
  }

  critical_path_seconds_.assign(nodes_.size(), 0);
  for (auto cursor = order.rbegin(); cursor != order.rend(); ++cursor) {
    std::uint64_t longest_tail = 0;
    for (const std::size_t dependent : dependents_[*cursor]) {
      longest_tail = std::max(longest_tail, critical_path_seconds_[dependent]);
    }
    critical_path_seconds_[*cursor] =
        saturating_add(nodes_[*cursor].spec.resources.est_seconds, longest_tail);
  }
}

std::size_t TaskGraph::size() const noexcept { return nodes_.size(); }

const TaskNode& TaskGraph::node(std::size_t index) const { return nodes_.at(index); }

std::optional<std::size_t> TaskGraph::find(std::string_view task_id) const {
  const auto found = index_by_id_.find(task_id);
  if (found == index_by_id_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::span<const std::size_t> TaskGraph::dependencies(std::size_t index) const {
  return dependencies_.at(index);
}

std::span<const std::size_t> TaskGraph::dependents(std::size_t index) const {
  return dependents_.at(index);
}

std::uint64_t TaskGraph::critical_path_seconds(std::size_t index) const {
  return critical_path_seconds_.at(index);
}

}  // namespace svp::exec
