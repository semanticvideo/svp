#include "lease_heartbeats.hpp"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

namespace svp::exec::detail {

LeaseHeartbeats::LeaseHeartbeats(std::function<void(const std::string&)> send)
    : send_(std::move(send)), thread_([this] { loop(); }) {}

LeaseHeartbeats::~LeaseHeartbeats() { stop(); }

void LeaseHeartbeats::add(const std::string& lease_id,
                          std::chrono::milliseconds interval) {
  const std::lock_guard lock(mutex_);
  leases_[lease_id] = interval;
  changed_.notify_all();
}

void LeaseHeartbeats::remove(const std::string& lease_id) {
  const std::lock_guard lock(mutex_);
  leases_.erase(lease_id);
  changed_.notify_all();
}

void LeaseHeartbeats::stop() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
    changed_.notify_all();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void LeaseHeartbeats::loop() {
  using SteadyTime = std::chrono::steady_clock::time_point;
  std::unique_lock lock(mutex_);
  std::optional<SteadyTime> due;
  while (!stopping_) {
    if (leases_.empty()) {
      due.reset();
      changed_.wait(lock);
      continue;
    }
    std::chrono::milliseconds interval = leases_.begin()->second;
    for (const auto& [lease_id, lease_interval] : leases_) {
      interval = std::min(interval, lease_interval);
    }
    const SteadyTime now = std::chrono::steady_clock::now();
    due = std::min(due.value_or(now + interval), now + interval);
    if (now < *due) {
      changed_.wait_until(lock, *due);
      continue;
    }
    std::vector<std::string> lease_ids;
    for (const auto& [lease_id, lease_interval] : leases_) {
      lease_ids.push_back(lease_id);
    }
    due = now + interval;
    lock.unlock();
    for (const std::string& lease_id : lease_ids) {
      try {
        send_(lease_id);
      } catch (const std::exception&) {
        // See class comment: the peer's reader reports the loss.
      }
    }
    lock.lock();
  }
}

}  // namespace svp::exec::detail
