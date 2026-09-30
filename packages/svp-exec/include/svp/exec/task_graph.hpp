#pragma once

#include "svp/exec/exec_error.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// Canonical position of a task's result among the results one reducer
// consumes (plan §4.5, RC2 §5.16 / §20.2: output order is normative and never
// completion order). `lane` names the reducer input stream
// ("ocr.vstream_000", "asr.astream_000"); `ordinals` is the position within
// it (sample index, window ordinal, chunk ordinal, ...), compared
// lexicographically. It is separate from task_id: IDs are stable names,
// order keys are what reducers sort by.
struct TaskOrderKey {
  std::string lane;
  std::vector<std::uint64_t> ordinals;

  bool operator==(const TaskOrderKey&) const = default;
};

// Lane first, then ordinals lexicographically.
[[nodiscard]] bool operator<(const TaskOrderKey& left, const TaskOrderKey& right);

struct TaskNode {
  TaskSpec spec;
  TaskOrderKey order_key;
};

enum class TaskGraphIssue {
  invalid_task_spec,
  duplicate_task_id,
  missing_dependency,
  dependency_cycle,
  duplicate_order_key,
  mixed_build_sessions,
};

[[nodiscard]] std::string_view task_graph_issue_name(TaskGraphIssue issue) noexcept;

// Thrown when a graph cannot be built. code() is invalid_value; issue() says
// which rule broke, and the message names the offending task.
class TaskGraphError : public ExecError {
 public:
  TaskGraphError(TaskGraphIssue issue, std::string message);
  [[nodiscard]] TaskGraphIssue issue() const noexcept;

 private:
  TaskGraphIssue issue_;
};

// Immutable DAG of TaskSpecs (RC2 §5.16, §20.1-§20.2). Construction validates
// every spec (validate_task_spec), one build_session_id across all nodes,
// unique task IDs, unique order keys, that every depends_on entry names a
// node, and acyclicity. Node indices follow the input order.
class TaskGraph {
 public:
  explicit TaskGraph(std::vector<TaskNode> nodes);

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const TaskNode& node(std::size_t index) const;
  [[nodiscard]] std::optional<std::size_t> find(std::string_view task_id) const;

  [[nodiscard]] std::span<const std::size_t> dependencies(std::size_t index) const;
  [[nodiscard]] std::span<const std::size_t> dependents(std::size_t index) const;

  // Sum of resources.est_seconds along the longest path from this task to
  // any task nothing depends on, this task included (saturating). The
  // scheduler runs the longest remaining chain first (plan §3.5).
  [[nodiscard]] std::uint64_t critical_path_seconds(std::size_t index) const;

 private:
  std::vector<TaskNode> nodes_;
  std::map<std::string, std::size_t, std::less<>> index_by_id_;
  std::vector<std::vector<std::size_t>> dependencies_;
  std::vector<std::vector<std::size_t>> dependents_;
  std::vector<std::uint64_t> critical_path_seconds_;
};

// Which tasks may run: a task is ready once every dependency has completed.
// Not thread-safe; owned by one scheduler run.
class ReadySet {
 public:
  explicit ReadySet(const TaskGraph& graph);

  // Tasks with no dependencies, ascending index.
  [[nodiscard]] std::vector<std::size_t> initially_ready() const;

  // Marks `index` complete and returns the dependents that became ready,
  // ascending index. Completing a task twice throws ExecError(invalid_value).
  std::vector<std::size_t> complete(std::size_t index);

  [[nodiscard]] bool is_complete(std::size_t index) const;
  [[nodiscard]] bool all_complete() const noexcept;

 private:
  const TaskGraph* graph_;
  std::vector<std::size_t> pending_dependencies_;
  std::vector<bool> complete_;
  std::size_t completed_count_ = 0;
};

}  // namespace svp::exec
