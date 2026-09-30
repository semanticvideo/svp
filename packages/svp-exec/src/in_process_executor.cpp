#include "svp/exec/in_process_executor.hpp"

#include "lease_heartbeats.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/task_attempt_runner.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace svp::exec {

struct InProcessExecutor::State {
  struct Assignment {
    TaskSpec spec;
    Lease lease;
  };

  State(const TaskTypeRegistry& registry_in, TaskArtifactAccess& artifacts_in,
        InProcessExecutorOptions options_in)
      : registry(registry_in), artifacts(artifacts_in), options(std::move(options_in)) {}

  void work() {
    while (true) {
      Assignment assignment;
      std::shared_ptr<CancellationToken> cancellation;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping) {
          return;
        }
        assignment = std::move(queue.front());
        queue.pop_front();
        cancellation = std::make_shared<CancellationToken>();
        running.emplace(assignment.lease.lease_id, cancellation);
        heartbeats->add(assignment.lease.lease_id, assignment.lease.heartbeat_interval);
      }
      AttemptOutput output = run_task_attempt(
          registry, artifacts, assignment.spec,
          AttemptContext{.attempt = assignment.lease.attempt,
                         .worker_session_id = options.worker_session_id,
                         .runtime_id = options.runtime_id},
          *cancellation);
      heartbeats->remove(assignment.lease.lease_id);
      bool report = false;
      {
        const std::lock_guard lock(mutex);
        report = !stopping && dropped.erase(assignment.lease.lease_id) == 0;
        running.erase(assignment.lease.lease_id);
      }
      if (report) {
        events->attempt_finished(assignment.lease.lease_id, std::move(output));
      }
    }
  }

  const TaskTypeRegistry& registry;
  TaskArtifactAccess& artifacts;
  InProcessExecutorOptions options;

  ExecutorEvents* events = nullptr;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Assignment> queue;
  // Running leases and each attempt's cancellation token.
  std::map<std::string, std::shared_ptr<CancellationToken>, std::less<>> running;
  std::set<std::string, std::less<>> dropped;
  bool stopping = false;
  std::optional<detail::LeaseHeartbeats> heartbeats;
  std::vector<std::thread> threads;
};

InProcessExecutor::InProcessExecutor(const TaskTypeRegistry& registry,
                                     TaskArtifactAccess& artifacts,
                                     InProcessExecutorOptions options)
    : state_(std::make_unique<State>(registry, artifacts, std::move(options))) {
  if (state_->options.threads == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "in-process executor needs at least one thread");
  }
}

InProcessExecutor::~InProcessExecutor() { stop(); }

std::string_view InProcessExecutor::id() const { return state_->options.executor_id; }

std::size_t InProcessExecutor::slots() const { return state_->options.threads; }

LossQuarantine InProcessExecutor::loss_quarantine() const { return LossQuarantine::never; }

void InProcessExecutor::start(ExecutorEvents& events) {
  State& state = *state_;
  state.events = &events;
  state.heartbeats.emplace(
      [&events](const std::string& lease_id) { events.lease_heartbeat(lease_id); });
  for (std::size_t index = 0; index < state.options.threads; ++index) {
    state.threads.emplace_back([&state] { state.work(); });
  }
}

void InProcessExecutor::assign(const TaskSpec& spec, const Lease& lease) {
  const std::lock_guard lock(state_->mutex);
  state_->queue.push_back(State::Assignment{.spec = spec, .lease = lease});
  state_->changed.notify_one();
}

void InProcessExecutor::cancel(std::string_view lease_id) {
  State& state = *state_;
  const std::lock_guard lock(state.mutex);
  const auto queued = std::find_if(state.queue.begin(), state.queue.end(),
                                   [&](const State::Assignment& assignment) {
                                     return assignment.lease.lease_id == lease_id;
                                   });
  if (queued != state.queue.end()) {
    state.queue.erase(queued);
    return;
  }
  if (const auto found = state.running.find(lease_id); found != state.running.end()) {
    state.dropped.emplace(lease_id);
    found->second->request();
  }
}

void InProcessExecutor::lease_expired(std::string_view lease_id) { cancel(lease_id); }

void InProcessExecutor::stop() {
  State& state = *state_;
  {
    const std::lock_guard lock(state.mutex);
    state.stopping = true;
    state.queue.clear();
    for (const auto& [lease_id, cancellation] : state.running) {
      cancellation->request();
    }
    state.changed.notify_all();
  }
  for (std::thread& thread : state.threads) {
    thread.join();
  }
  state.threads.clear();
  if (state.heartbeats) {
    state.heartbeats->stop();
  }
}

}  // namespace svp::exec
