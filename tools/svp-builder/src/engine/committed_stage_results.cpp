#include "engine/committed_stage_results.hpp"

#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"

#include <stdexcept>

namespace svp::builder::engine {

void CommittedStageResults::record(const std::string& task_id,
                                   const StageTaskProducts& products) {
  Entry entry;
  for (const auto& [name, bytes] : products.states) {
    entry.states.emplace(name, bytes);
  }
  for (const StagedEntry& staged : products.staging) {
    entry.capture.emplace(staged.relative_path,
                          staged.kind == StagedEntryKind::file
                              ? std::optional(svp::exec::blake3_digest(staged.bytes))
                              : std::nullopt);
  }
  const std::lock_guard lock(mutex_);
  entries_.insert_or_assign(task_id, std::move(entry));
}

bool CommittedStageResults::contains(std::string_view task_id) const {
  const std::lock_guard lock(mutex_);
  return entries_.find(task_id) != entries_.end();
}

bool CommittedStageResults::has_state(std::string_view task_id,
                                      std::string_view name) const {
  const std::lock_guard lock(mutex_);
  const auto found = entries_.find(task_id);
  return found != entries_.end() && found->second.states.find(name) != found->second.states.end();
}

const CommittedStageResults::Entry& CommittedStageResults::entry(
    std::string_view task_id) const {
  const auto found = entries_.find(task_id);
  if (found == entries_.end()) {
    throw std::runtime_error("no committed result for task `" + std::string(task_id) +
                             "`");
  }
  return found->second;
}

std::vector<std::byte> CommittedStageResults::state(std::string_view task_id,
                                                    std::string_view name) const {
  const std::lock_guard lock(mutex_);
  const Entry& found = entry(task_id);
  const auto state = found.states.find(name);
  if (state == found.states.end()) {
    throw std::runtime_error("task `" + std::string(task_id) + "` has no state `" +
                             std::string(name) + "`");
  }
  return state->second;
}

nlohmann::json CommittedStageResults::json_state(std::string_view task_id,
                                                 std::string_view name) const {
  return parse_json_state(state(task_id, name));
}

StagingCaptureDigests CommittedStageResults::capture(std::string_view task_id) const {
  const std::lock_guard lock(mutex_);
  return entry(task_id).capture;
}

void CommittedStageResults::record_task_output(const std::string& task_id,
                                               std::vector<std::byte> bytes) {
  const std::lock_guard lock(mutex_);
  task_outputs_.insert_or_assign(task_id, std::move(bytes));
}

std::vector<std::byte> CommittedStageResults::task_output(std::string_view task_id) const {
  const std::lock_guard lock(mutex_);
  const auto found = task_outputs_.find(task_id);
  if (found == task_outputs_.end()) {
    throw std::runtime_error("no committed output for task `" + std::string(task_id) + "`");
  }
  return found->second;
}

void record_committed_result(CommittedStageResults& results, const svp::exec::TaskSpec& spec,
                             const svp::exec::CommittedResult& committed) {
  if (spec.task_type == svp::vision::tasks::kOcrFrameBatchTaskType ||
      spec.task_type == svp::vision::tasks::kTrackWindowTaskType) {
    if (committed.payloads.size() != 1) {
      throw std::runtime_error(spec.task_type + " task `" + spec.task_id + "` committed " +
                               std::to_string(committed.payloads.size()) + " outputs, not one");
    }
    results.record_task_output(spec.task_id, committed.payloads.front());
    return;
  }
  results.record(spec.task_id,
                 decode_stage_products(committed.result.outputs, committed.payloads));
}

StageResultCommitSink::StageResultCommitSink(svp::exec::ResultCommitSink& journal_sink,
                                             CommittedStageResults& results)
    : journal_sink_(journal_sink), results_(results) {}

void StageResultCommitSink::commit(const svp::exec::TaskSpec& spec,
                                   const svp::exec::CommittedResult& committed) {
  journal_sink_.commit(spec, committed);
  record_committed_result(results_, spec, committed);
}

}  // namespace svp::builder::engine
