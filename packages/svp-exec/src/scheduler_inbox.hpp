#pragma once

#include "svp/exec/executor.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace svp::exec::detail {

struct HeartbeatEvent {
  std::string lease_id;
};

struct FinishedEvent {
  std::string lease_id;
  AttemptOutput output;
};

struct FailedEvent {
  std::string lease_id;
  AttemptFailureKind kind = AttemptFailureKind::executor_lost;
  std::string message;
};

using InboxEvent = std::variant<HeartbeatEvent, FinishedEvent, FailedEvent>;

// Thread-safe queue from executor threads to the scheduler thread. Every
// scheduler decision is made on one thread from these events, so scheduler
// state needs no locking.
class SchedulerInbox final : public ExecutorEvents {
 public:
  void lease_heartbeat(std::string_view lease_id) override;
  void attempt_finished(std::string_view lease_id, AttemptOutput output) override;
  void attempt_failed(std::string_view lease_id, AttemptFailureKind kind,
                      std::string message) override;

  // Waits up to `timeout` (real time) for at least one event, then returns
  // every queued event in arrival order.
  [[nodiscard]] std::vector<InboxEvent> wait(std::chrono::milliseconds timeout);

 private:
  void push(InboxEvent event);

  std::mutex mutex_;
  std::condition_variable arrived_;
  std::deque<InboxEvent> events_;
};

}  // namespace svp::exec::detail
