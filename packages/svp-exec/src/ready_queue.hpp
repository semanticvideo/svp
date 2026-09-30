#pragma once

#include "svp/exec/task_graph.hpp"

#include <cstddef>
#include <set>

namespace svp::exec::detail {

// Ready tasks in dispatch order (plan §3.5 "ordered by critical path", §4.4
// "ordered by canonical position"): longest critical path first, then
// canonical order key. Order keys are unique within a graph, so the order is
// total and independent of when tasks became ready.
class ReadyQueue {
 public:
  explicit ReadyQueue(const TaskGraph& graph) : tasks_(Before{&graph}) {}

  void insert(std::size_t task) { tasks_.insert(task); }
  void erase(std::size_t task) { tasks_.erase(task); }
  [[nodiscard]] bool empty() const noexcept { return tasks_.empty(); }
  [[nodiscard]] auto begin() const { return tasks_.begin(); }
  [[nodiscard]] auto end() const { return tasks_.end(); }

 private:
  struct Before {
    const TaskGraph* graph;
    bool operator()(std::size_t left, std::size_t right) const {
      const auto left_path = graph->critical_path_seconds(left);
      const auto right_path = graph->critical_path_seconds(right);
      if (left_path != right_path) {
        return left_path > right_path;
      }
      return graph->node(left).order_key < graph->node(right).order_key;
    }
  };

  std::set<std::size_t, Before> tasks_;
};

}  // namespace svp::exec::detail
