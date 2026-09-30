#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace svp::exec {

struct InProcessExecutorOptions {
  std::string executor_id = "in-process";
  // Worker threads, and therefore advertised slots. Required (>= 1) and
  // deliberately without a default: slot counts come from the machine's
  // measured capacity (plan §3.5 calibration), never from code, so the same
  // build runs on any Apple Silicon chip, core count, or memory size.
  std::size_t threads = 0;
  // Stamped into TaskResult.execution.
  std::string worker_session_id = "ws_in_process";
  Blake3Digest runtime_id{};
};

// Runs registry task functions on a fixed pool of threads in this process
// through run_task_attempt, the same path the worker loop uses. Heartbeats
// are sent while a task runs. cancel() drops a queued lease, or sets a running
// attempt's CancellationToken and discards its result; task functions stop
// only at their own cooperative checks, so stop() (which cancels every running
// attempt) waits for them to return.
//
// loss_quarantine() is LossQuarantine::never: this executor is the
// coordinator's own process, so losing it to quarantine never routes work
// anywhere healthier, and in a one-Mac build it is the only executor.
class InProcessExecutor final : public Executor {
 public:
  InProcessExecutor(const TaskTypeRegistry& registry, TaskArtifactAccess& artifacts,
                    InProcessExecutorOptions options);
  ~InProcessExecutor() override;
  InProcessExecutor(const InProcessExecutor&) = delete;
  InProcessExecutor& operator=(const InProcessExecutor&) = delete;

  [[nodiscard]] std::string_view id() const override;
  [[nodiscard]] std::size_t slots() const override;
  [[nodiscard]] LossQuarantine loss_quarantine() const override;
  void start(ExecutorEvents& events) override;
  void assign(const TaskSpec& spec, const Lease& lease) override;
  void cancel(std::string_view lease_id) override;
  void lease_expired(std::string_view lease_id) override;
  void stop() override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace svp::exec
