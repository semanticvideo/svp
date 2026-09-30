#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace svp::exec::detail {

// Sends a heartbeat for every registered lease on a fixed cadence (the
// shortest registered interval) from its own thread (plan §4.4: heartbeats
// renew leases). Registering or removing a lease never postpones a due
// heartbeat. `send` runs on the heartbeat thread without the internal lock
// held; exceptions from it are swallowed (a vanished peer is noticed by its
// reader, not here).
class LeaseHeartbeats {
 public:
  explicit LeaseHeartbeats(std::function<void(const std::string& lease_id)> send);
  ~LeaseHeartbeats();
  LeaseHeartbeats(const LeaseHeartbeats&) = delete;
  LeaseHeartbeats& operator=(const LeaseHeartbeats&) = delete;

  void add(const std::string& lease_id, std::chrono::milliseconds interval);
  void remove(const std::string& lease_id);
  // Joins the thread; idempotent.
  void stop();

 private:
  void loop();

  std::function<void(const std::string&)> send_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::map<std::string, std::chrono::milliseconds> leases_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace svp::exec::detail
