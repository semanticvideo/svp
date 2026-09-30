// record_task_graph and resume_scheduler_state: the journal side of a
// resumable scheduler run.

#include "svp/exec/journal_scheduler_resume.hpp"

#include "svp/exec/exec_error.hpp"

#include <map>
#include <string>

namespace svp::exec {
namespace {

void require_session(const RecoveryJournal& journal, const TaskSpec& spec) {
  if (spec.build_session_id != journal.build_session_id()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task `" + spec.task_id + "` belongs to build session `" +
                        spec.build_session_id + "`, the journal to `" +
                        journal.build_session_id() + "`");
  }
}

JournalTaskRecord task_row(const TaskSpec& spec) {
  return JournalTaskRecord{.task_id = spec.task_id,
                           .task_type = spec.task_type,
                           .cache_key = spec.cache_key,
                           .depends_on = spec.depends_on};
}

[[noreturn]] void graph_mismatch(const std::string& task_id, std::string_view what) {
  throw ExecError(ExecErrorCode::invalid_value,
                  "the recovery journal does not describe this task graph: task `" + task_id +
                      "` " + std::string(what) + "; start a new build instead of resuming");
}

// A journaled task must be the same unit of work as its graph node, or its
// committed result would be reused for something else.
void require_same_task(const ResumedTask& journaled, const TaskSpec& spec) {
  if (journaled.task_type != spec.task_type) {
    graph_mismatch(spec.task_id, "has another task type");
  }
  // Both sides are sorted: TaskSpec requires ascending depends_on and the
  // journal reads dependencies ordered.
  if (journaled.depends_on != spec.depends_on) {
    graph_mismatch(spec.task_id, "has other dependencies");
  }
  if (journaled.cache_key != spec.cache_key) {
    graph_mismatch(spec.task_id, "has another cache key");
  }
}

}  // namespace

void record_task_graph(RecoveryJournal& journal, const TaskGraph& graph) {
  for (std::size_t index = 0; index < graph.size(); ++index) {
    require_session(journal, graph.node(index).spec);
  }
  for (std::size_t index = 0; index < graph.size(); ++index) {
    journal.record_task(task_row(graph.node(index).spec));
  }
}

SchedulerResumeState resume_scheduler_state(
    RecoveryJournal& journal, const TaskGraph& graph,
    std::span<const SourceFingerprintRecord> current_sources) {
  for (std::size_t index = 0; index < graph.size(); ++index) {
    require_session(journal, graph.node(index).spec);
  }
  SchedulerResumeState state;
  state.report = journal.resume(current_sources);

  std::map<std::string, const ResumedTask*, std::less<>> journaled;
  for (const ResumedTask& task : state.report.tasks) {
    const std::optional<std::size_t> index = graph.find(task.task_id);
    if (!index) {
      graph_mismatch(task.task_id, "is journaled but not planned");
    }
    require_same_task(task, graph.node(*index).spec);
    journaled.emplace(task.task_id, &task);
  }
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const TaskSpec& spec = graph.node(index).spec;
    if (!journaled.contains(spec.task_id)) {
      journal.record_task(task_row(spec));
    }
  }
  state.committed = load_committed_results(journal, graph);
  return state;
}

}  // namespace svp::exec
