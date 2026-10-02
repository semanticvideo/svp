#include "engine/build_task_graph.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace svp::builder::engine {

svp::exec::TaskGraph make_split_build_task_graph(const std::vector<PlannedStageTask>& tasks,
                                                 const std::string& build_session_id,
                                                 const std::string& build_inputs_blake3,
                                                 const SplitStageTasks& split) {
  const svp::exec::TaskGraph stages =
      make_stage_task_graph(tasks, build_session_id, build_inputs_blake3);
  std::map<std::string, const std::vector<svp::exec::TaskNode>*, std::less<>> subtasks;
  if (split.ocr_batches != nullptr) {
    subtasks.emplace(std::string(stage_task_id(StageTaskKind::ocr)), &split.ocr_batches->nodes);
  }
  if (split.tracking_windows != nullptr) {
    subtasks.emplace(std::string(stage_task_id(StageTaskKind::tracking)),
                     &split.tracking_windows->nodes);
  }
  if (subtasks.empty()) {
    return stages;
  }
  std::vector<svp::exec::TaskNode> nodes;
  for (std::size_t index = 0; index < stages.size(); ++index) {
    svp::exec::TaskNode node = stages.node(index);
    if (const auto found = subtasks.find(node.spec.task_id); found != subtasks.end()) {
      // Subtasks run where the stage would have started; the reducer waits
      // for all of them.
      nodes.insert(nodes.end(), found->second->begin(), found->second->end());
      for (const svp::exec::TaskNode& subtask : *found->second) {
        node.spec.depends_on.push_back(subtask.spec.task_id);
      }
      std::sort(node.spec.depends_on.begin(), node.spec.depends_on.end());
    }
    nodes.push_back(std::move(node));
  }
  return svp::exec::TaskGraph(std::move(nodes));
}

}  // namespace svp::builder::engine
