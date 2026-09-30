#pragma once

#include "svp/exec/executor.hpp"
#include "svp/exec/frame_limits.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// After SHUTDOWN and closing its input, how long a worker process may take to
// exit before it is killed (plan §4.4: "if a session does not stop within a
// grace period, the agent terminates the session process"). Task functions are
// not interruptible, so this bounds how long cancel or stop can wait on one;
// 2 s lets a worker that is between tasks exit cleanly and is short enough
// that Ctrl-C still feels immediate.
inline constexpr std::chrono::milliseconds kDefaultWorkerShutdownGrace{2'000};

struct LoopbackExecutorOptions {
  std::string executor_id = "loopback";
  // A program that serves run_worker_loop on its stdin/stdout.
  std::filesystem::path worker_executable;
  std::vector<std::string> worker_arguments;
  // Leases the worker process runs at once. Required (>= 1) and deliberately
  // without a default: it is the worker's advertised or measured capacity,
  // never a count baked into code.
  std::size_t slots = 0;
  std::chrono::milliseconds shutdown_grace = kDefaultWorkerShutdownGrace;
  FrameLimits frame_limits{};
};

// Runs tasks in a child worker process connected by a socket pair, speaking
// the same framed protocol a remote worker will (plan §3.1, §4.3): ASSIGN
// with lease, HEARTBEAT, RESULT, CANCEL, SHUTDOWN.
//
//   * Results are decoded with task_result_from_result_frame, which verifies
//     the schema, output_digest, and every payload's length and BLAKE3. Bytes
//     that fail any check, a result for a lease this executor did not issue,
//     or any other unexpected frame end the session: its outstanding leases
//     fail with invalid_result and the process is killed.
//   * End of stream (the process crashed or exited), including one that ends
//     inside a frame, fails outstanding leases with executor_lost. The next
//     assign() starts a fresh process.
//   * lease_expired() means the session stopped heartbeating; the process is
//     presumed stalled and is killed (its other leases fail as lost).
class LoopbackExecutor final : public Executor {
 public:
  explicit LoopbackExecutor(LoopbackExecutorOptions options);
  ~LoopbackExecutor() override;
  LoopbackExecutor(const LoopbackExecutor&) = delete;
  LoopbackExecutor& operator=(const LoopbackExecutor&) = delete;

  [[nodiscard]] std::string_view id() const override;
  [[nodiscard]] std::size_t slots() const override;
  void start(ExecutorEvents& events) override;
  void assign(const TaskSpec& spec, const Lease& lease) override;
  void cancel(std::string_view lease_id) override;
  void lease_expired(std::string_view lease_id) override;
  void stop() override;

  // Worker processes started so far (tests observe respawns).
  [[nodiscard]] std::size_t processes_started() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace svp::exec
