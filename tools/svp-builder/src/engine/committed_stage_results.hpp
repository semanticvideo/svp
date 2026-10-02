#pragma once

// The committed results of a build's stage tasks, as later tasks and the final
// assembly read them: each task's named states, and digests of the staging
// entries it captured (for the end-of-build capture check). Populated by
// StageResultCommitSink as tasks commit and, on --resume, from the journal.
// Dependent tasks only read tasks they declare in depends_on, which the
// scheduler commits before they start.

#include "engine/stage_task_products.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/result_commit_sink.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::engine {

// Digest of each captured file, and each captured directory (empty optional).
using StagingCaptureDigests = std::map<std::string, std::optional<svp::exec::Blake3Digest>>;

class CommittedStageResults {
 public:
  // Keeps `products`' states and the digests of its staging capture.
  void record(const std::string& task_id, const StageTaskProducts& products);

  [[nodiscard]] bool contains(std::string_view task_id) const;
  [[nodiscard]] bool has_state(std::string_view task_id, std::string_view name) const;
  // Throws std::runtime_error when the task or state is missing.
  [[nodiscard]] std::vector<std::byte> state(std::string_view task_id,
                                             std::string_view name) const;
  [[nodiscard]] nlohmann::json json_state(std::string_view task_id,
                                          std::string_view name) const;
  [[nodiscard]] StagingCaptureDigests capture(std::string_view task_id) const;

  // Results of tasks that are not whole stages (ocr.frame_batch,
  // track.window): their one output's bytes, as the reducer that consumes
  // them reads them.
  void record_task_output(const std::string& task_id, std::vector<std::byte> bytes);
  // Throws std::runtime_error when the task's output was never recorded.
  [[nodiscard]] std::vector<std::byte> task_output(std::string_view task_id) const;

 private:
  struct Entry {
    std::map<std::string, std::vector<std::byte>, std::less<>> states;
    StagingCaptureDigests capture;
  };
  const Entry& entry(std::string_view task_id) const;

  mutable std::mutex mutex_;
  std::map<std::string, Entry, std::less<>> entries_;
  std::map<std::string, std::vector<std::byte>, std::less<>> task_outputs_;
};

// Records one committed result where its consumers read it: a whole-stage
// task's states and staging capture, or a frame-batch or window task's output
// bytes.
void record_committed_result(CommittedStageResults& results, const svp::exec::TaskSpec& spec,
                             const svp::exec::CommittedResult& committed);

// Commits to the journal first (RC2 §20.4: a result is committed only once it
// is durable), then makes the result visible to dependents.
class StageResultCommitSink final : public svp::exec::ResultCommitSink {
 public:
  StageResultCommitSink(svp::exec::ResultCommitSink& journal_sink,
                        CommittedStageResults& results);
  void commit(const svp::exec::TaskSpec& spec,
              const svp::exec::CommittedResult& committed) override;

 private:
  svp::exec::ResultCommitSink& journal_sink_;
  CommittedStageResults& results_;
};

}  // namespace svp::builder::engine
