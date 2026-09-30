#pragma once

// Shared helpers for the task graph, scheduler, and loopback tests.

#include "exec_test_support.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/ordered_reduction.hpp"
#include "svp/exec/scheduler.hpp"
#include "toy_tasks.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace svp::exec::test {

// Real-time cadence of heartbeats in tests. Leases use the default 30 s floor
// on a ManualClock, so they expire only when a test advances the clock.
inline constexpr std::chrono::milliseconds kTestHeartbeatInterval{20};
// Scheduler wake-up bound in tests; keeps cancellation and clock advances
// visible within a few milliseconds.
inline constexpr std::chrono::milliseconds kTestMaxIdleWait{5};
// Upper bound for any wait on an asynchronous condition in a test; far above
// what a healthy run needs, so hitting it means a hang.
inline constexpr std::chrono::milliseconds kTestWaitLimit{20'000};

inline SchedulerPolicy test_policy() {
  SchedulerPolicy policy;
  policy.lease.heartbeat_interval = kTestHeartbeatInterval;
  policy.max_idle_wait = kTestMaxIdleWait;
  return policy;
}

inline std::chrono::milliseconds test_lease_length(std::uint64_t est_seconds = 1) {
  return lease_duration(test_policy().lease, est_seconds);
}

// Thread-safe record of observer events.
class EventLog {
 public:
  AttemptObserver observer() {
    return [this](const AttemptEvent& event) {
      const std::lock_guard lock(mutex_);
      events_.push_back(event);
      changed_.notify_all();
    };
  }

  std::vector<AttemptEvent> events() const {
    const std::lock_guard lock(mutex_);
    return events_;
  }

  std::size_t count(AttemptEventKind kind) const {
    const std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        events_.begin(), events_.end(),
        [&](const AttemptEvent& event) { return event.kind == kind; }));
  }

  // Blocks until `predicate(events)` holds; throws after `limit`.
  void wait_until(const std::function<bool(const std::vector<AttemptEvent>&)>& predicate,
                  std::chrono::milliseconds limit = kTestWaitLimit) {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, limit, [&] { return predicate(events_); })) {
      throw std::runtime_error("timed out waiting for scheduler events");
    }
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<AttemptEvent> events_;
};

struct TemporaryDirectory {
  std::filesystem::path path;

  explicit TemporaryDirectory(std::string_view name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           (std::string(name) + "-" + std::to_string(nonce));
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

// A toy graph with two reducer lanes and cross-lane dependencies:
//   lane "alpha": a_000 .. a_{n-1}, each a_i (i >= 2) depends on a_{i-2}
//   lane "beta":  b_000 .. b_{n-1}, each b_i depends on a_i
inline std::vector<ToyTask> two_lane_toy_tasks(std::size_t per_lane) {
  std::vector<ToyTask> tasks;
  const auto name = [](char lane, std::size_t index) {
    std::string digits = std::to_string(index);
    return std::string("task.toy.") + lane + "_" + std::string(3 - digits.size(), '0') + digits;
  };
  for (std::size_t index = 0; index < per_lane; ++index) {
    ToyTask alpha{.task_id = name('a', index),
                  .seed = 1000 + index,
                  .order_key = {.lane = "alpha", .ordinals = {index}}};
    if (index >= 2) {
      alpha.depends_on = {name('a', index - 2)};
    }
    tasks.push_back(alpha);
    tasks.push_back(ToyTask{.task_id = name('b', index),
                            .depends_on = {name('a', index)},
                            .seed = 2000 + index,
                            .order_key = {.lane = "beta", .ordinals = {index}}});
  }
  return tasks;
}

inline TaskGraph make_toy_graph(const std::vector<ToyTask>& tasks) {
  std::vector<TaskNode> nodes;
  for (const ToyTask& task : tasks) {
    nodes.push_back(make_toy_node(task));
  }
  return TaskGraph(std::move(nodes));
}

// Reducer stand-in: concatenates a lane's payloads in canonical order and
// digests them.
inline std::string reduce_lane(const TaskGraph& graph, std::string_view lane,
                               const std::vector<CommittedResult>& results) {
  std::string concatenated;
  for (const CommittedResult* committed :
       results_in_canonical_order(graph, lane, results)) {
    for (const FramePayload& payload : committed->payloads) {
      concatenated += to_text(payload);
    }
  }
  return blake3_hex(blake3_digest(concatenated));
}

inline void expect_succeeded(const BuildOutcome& outcome, std::string_view message) {
  if (outcome.status != BuildStatus::succeeded) {
    throw std::runtime_error(
        std::string(message) + ": build " + std::string(build_status_name(outcome.status)) +
        (outcome.failure ? " (" + std::string(build_failure_kind_name(outcome.failure->kind)) +
                               ": " + outcome.failure->message + ")"
                         : std::string()));
  }
}

// In-process toy runtime: registry plus store, faults ignored.
struct ToyRuntime {
  InMemoryArtifactStore store;
  TaskTypeRegistry registry;

  explicit ToyRuntime(ToyTaskOptions options = {}) {
    register_toy_tasks(registry, store, options);
  }
};

}  // namespace svp::exec::test
