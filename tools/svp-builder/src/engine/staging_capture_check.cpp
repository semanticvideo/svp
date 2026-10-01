#include "engine/staging_capture_check.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace svp::builder::engine {
namespace {

svp::exec::Blake3Digest file_digest(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::string content{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  return svp::exec::blake3_digest(std::string_view(content));
}

}  // namespace

std::vector<std::string> unreproducible_staging_entries(
    const std::filesystem::path& staging_dir, const std::vector<PlannedStageTask>& tasks,
    const CommittedStageResults& results) {
  std::vector<std::string> problems;
  for (const std::string& entry : list_staging_entries(staging_dir)) {
    const PlannedStageTask* owner = nullptr;
    for (const PlannedStageTask& task : tasks) {
      if (scope_covers(task.scope, entry)) {
        owner = &task;
      }
    }
    if (owner == nullptr) {
      // Directories that hold other entries are recreated by restoring those
      // entries; an empty one would be lost.
      std::error_code error;
      if (!entry.ends_with('/') || std::filesystem::is_empty(staging_dir / entry, error)) {
        problems.push_back(entry + ": outside every task's staging scope");
      }
      continue;
    }
    const std::string task_id(stage_task_id(owner->kind));
    const StagingCaptureDigests capture = results.capture(task_id);
    const auto captured = capture.find(entry);
    if (captured == capture.end()) {
      problems.push_back(entry + ": not captured by " + task_id);
    } else if (captured->second && *captured->second != file_digest(staging_dir / entry)) {
      problems.push_back(entry + ": changed after " + task_id + " captured it");
    }
  }
  return problems;
}

}  // namespace svp::builder::engine
