#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// The lease that accompanies one ASSIGN (plan §4.3: TaskSpec plus lease
// {attempt, lease_id, expires_in_ms}). heartbeat_interval tells the worker how
// often to renew; it comes from the scheduler's LeasePolicy.
struct Lease {
  std::string lease_id;
  std::uint64_t attempt = 0;
  std::chrono::milliseconds duration{0};
  std::chrono::milliseconds heartbeat_interval{0};

  bool operator==(const Lease&) const = default;
};

// What one attempt produced: the TaskResult (succeeded or failed) and, for a
// success, the bytes of each output in output order.
struct AttemptOutput {
  TaskResult result;
  std::vector<FramePayload> payloads;
};

enum class AttemptFailureKind {
  // The executor received bytes it could not accept as a result (bad frame,
  // payload hash, schema, or digest). Quarantines the executor (plan §4.4).
  invalid_result,
  // The worker process died, disconnected, or could not be started. The
  // attempt is lost; the task is retried.
  executor_lost,
};

[[nodiscard]] std::string_view attempt_failure_kind_name(
    AttemptFailureKind kind) noexcept;

// Callbacks from an executor to the scheduler. Implementations are
// thread-safe and never block for long; executors call them from their own
// threads. Every lease handed to Executor::assign ends in exactly one
// attempt_finished or attempt_failed unless the scheduler has already
// abandoned it (cancel, expiry, stop), in which case reporting is optional.
class ExecutorEvents {
 public:
  virtual ~ExecutorEvents() = default;
  virtual void lease_heartbeat(std::string_view lease_id) = 0;
  virtual void attempt_finished(std::string_view lease_id, AttemptOutput output) = 0;
  virtual void attempt_failed(std::string_view lease_id, AttemptFailureKind kind,
                              std::string message) = 0;
};

// Whether lost attempts (executor_lost, lease expiry, hard deadline) count
// toward quarantining an executor (plan §4.4 "a worker with repeated failures
// is quarantined"). An invalid result quarantines every executor at once,
// whatever this says: bad bytes are never a transient condition.
enum class LossQuarantine {
  // RetryPolicy.quarantine_after_executor_failures losses quarantine the
  // executor. For executors in another process or on another machine, where
  // repeated losses point at a sick worker that other executors can route
  // around.
  after_repeated_losses,
  // Losses never quarantine. For the coordinator's own in-process executor:
  // its "losses" are stalls of the coordinator process itself or task
  // functions stuck past their deadline, which no other slot in the same
  // process avoids, and quarantining it in a one-Mac build (the only executor,
  // a fully supported configuration) would turn transient losses into a
  // guaranteed no_usable_executor failure. RetryPolicy.max_attempts still
  // bounds each task, so a task that keeps getting lost fails the build by
  // name instead.
  never,
};

[[nodiscard]] std::string_view loss_quarantine_name(LossQuarantine policy) noexcept;

// A place where tasks run (plan §3.1: in-process, loopback, remote).
// Executors pull work by advertising slots; the scheduler never has more than
// slots() leases outstanding on one executor. Nothing assumes how many
// executors exist or what hardware they run on: one in-process executor alone
// is a complete configuration, and results never depend on which executor, or
// how many, ran a task. All methods are called from the scheduler thread.
class Executor {
 public:
  virtual ~Executor() = default;

  // Stable, unique within a scheduler run; used in traces and quarantine.
  [[nodiscard]] virtual std::string_view id() const = 0;
  [[nodiscard]] virtual std::size_t slots() const = 0;
  // See LossQuarantine. Fixed for the executor's lifetime.
  [[nodiscard]] virtual LossQuarantine loss_quarantine() const {
    return LossQuarantine::after_repeated_losses;
  }

  // Called once before any assign(). `events` outlives stop().
  virtual void start(ExecutorEvents& events) = 0;

  // Starts one attempt; must not block on the task itself.
  virtual void assign(const TaskSpec& spec, const Lease& lease) = 0;

  // Cooperative cancellation of one lease (plan §4.3 CANCEL): the attempt's
  // CancellationToken is set so its task function can stop at its next safe
  // point. A result may still arrive afterwards; the scheduler ignores it.
  virtual void cancel(std::string_view lease_id) = 0;

  // The scheduler stopped waiting for this lease because it expired. The
  // executor decides what that says about the worker (the loopback executor
  // treats a session that stopped heartbeating as stalled and replaces it).
  virtual void lease_expired(std::string_view lease_id) = 0;

  // Stops all work and releases threads and processes; no events are
  // delivered after it returns. Idempotent.
  virtual void stop() = 0;
};

}  // namespace svp::exec
