#include "svp/exec/ordered_reduction.hpp"

#include "svp/exec/exec_error.hpp"

#include <algorithm>
#include <utility>

namespace svp::exec {

std::vector<const CommittedResult*> results_in_canonical_order(
    const TaskGraph& graph, std::string_view lane, std::span<const CommittedResult> results) {
  std::vector<const CommittedResult*> by_task(graph.size(), nullptr);
  for (const CommittedResult& committed : results) {
    const auto index = graph.find(committed.result.task_id);
    if (!index) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "committed result for unknown task `" + committed.result.task_id + "`");
    }
    if (by_task[*index] != nullptr) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task `" + committed.result.task_id + "` has more than one result");
    }
    by_task[*index] = &committed;
  }

  std::vector<std::pair<const TaskOrderKey*, const CommittedResult*>> lane_results;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const TaskNode& node = graph.node(index);
    if (node.order_key.lane != lane) {
      continue;
    }
    if (by_task[index] == nullptr) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task `" + node.spec.task_id + "` in lane `" + std::string(lane) +
                          "` has no committed result");
    }
    lane_results.emplace_back(&node.order_key, by_task[index]);
  }
  std::sort(lane_results.begin(), lane_results.end(),
            [](const auto& left, const auto& right) { return *left.first < *right.first; });

  std::vector<const CommittedResult*> ordered;
  ordered.reserve(lane_results.size());
  for (const auto& [key, committed] : lane_results) {
    ordered.push_back(committed);
  }
  return ordered;
}

}  // namespace svp::exec
