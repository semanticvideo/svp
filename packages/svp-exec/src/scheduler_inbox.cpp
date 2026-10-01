#include "scheduler_inbox.hpp"

#include <iterator>
#include <utility>

namespace svp::exec::detail {

void SchedulerInbox::lease_heartbeat(std::string_view lease_id) {
  push(HeartbeatEvent{.lease_id = std::string(lease_id)});
}

void SchedulerInbox::attempt_finished(std::string_view lease_id, AttemptOutput output) {
  push(FinishedEvent{.lease_id = std::string(lease_id), .output = std::move(output)});
}

void SchedulerInbox::attempt_failed(std::string_view lease_id, AttemptFailureKind kind,
                                    std::string message) {
  push(FailedEvent{
      .lease_id = std::string(lease_id), .kind = kind, .message = std::move(message)});
}

void SchedulerInbox::attempt_rejected(std::string_view lease_id, std::string code,
                                      std::string message) {
  push(RejectedEvent{
      .lease_id = std::string(lease_id), .code = std::move(code), .message = std::move(message)});
}

void SchedulerInbox::push(InboxEvent event) {
  const std::lock_guard lock(mutex_);
  events_.push_back(std::move(event));
  arrived_.notify_one();
}

std::vector<InboxEvent> SchedulerInbox::wait(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  arrived_.wait_for(lock, timeout, [&] { return !events_.empty(); });
  std::vector<InboxEvent> drained(std::make_move_iterator(events_.begin()),
                                  std::make_move_iterator(events_.end()));
  events_.clear();
  return drained;
}

}  // namespace svp::exec::detail
