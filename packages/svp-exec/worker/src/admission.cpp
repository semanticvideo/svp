#include "svp/exec/worker/admission.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace svp::exec::worker {
namespace {

std::string mib(std::uint64_t bytes) {
  return std::to_string(bytes / (1024 * 1024)) + " MiB";
}

}  // namespace

std::uint64_t AdmissionPolicy::reserve_bytes(std::uint64_t physical_memory_bytes) const noexcept {
  const std::uint64_t fraction =
      reserve_physical_divisor == 0 ? 0 : physical_memory_bytes / reserve_physical_divisor;
  return std::max(reserve_floor_bytes, fraction);
}

AdmissionDecision decide_admission(const AdmissionPolicy& policy,
                                   std::uint64_t physical_memory_bytes,
                                   const MemorySnapshot& memory, std::uint64_t pending,
                                   std::uint64_t required_bytes) {
  if (static_cast<int>(memory.pressure) >= static_cast<int>(policy.refuse_at)) {
    return AdmissionDecision{
        .admitted = false,
        .code = std::string(kRejectMemoryPressure),
        .message = "the worker is under " + std::string(memory_pressure_name(memory.pressure)) +
                   " memory pressure"};
  }
  const std::uint64_t reserve = policy.reserve_bytes(physical_memory_bytes);
  const std::uint64_t spoken_for = reserve + pending;
  const std::uint64_t free_for_task =
      memory.available_bytes > spoken_for ? memory.available_bytes - spoken_for : 0;
  const bool fits = required_bytes == 0 ? memory.available_bytes > spoken_for
                                        : free_for_task >= required_bytes;
  if (!fits) {
    return AdmissionDecision{
        .admitted = false,
        .code = std::string(kRejectInsufficientMemory),
        .message = "task needs " + mib(required_bytes) + " but the worker has " +
                   mib(memory.available_bytes) + " available, keeps a " + mib(reserve) +
                   " reserve, and expects " + mib(pending) +
                   " more for running tasks than they already use"};
  }
  return AdmissionDecision{.admitted = true, .code = {}, .message = {}};
}

std::uint64_t pending_bytes(std::uint64_t estimated_bytes,
                            std::optional<std::uint64_t> in_use) noexcept {
  if (!in_use) {
    return estimated_bytes;
  }
  return estimated_bytes > *in_use ? estimated_bytes - *in_use : 0;
}

AdmissionLedger::AdmissionLedger(AdmissionPolicy policy, std::uint64_t physical_memory_bytes,
                                 MemoryTurnPolicy turns, Clock clock)
    : policy_(policy),
      physical_memory_bytes_(physical_memory_bytes),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })),
      turns_(turns) {}

AdmissionDecision AdmissionLedger::try_admit(const LeaseAdmission& lease,
                                             const MemorySnapshot& memory,
                                             const SessionInUse& in_use) {
  const std::lock_guard lock(mutex_);
  const auto now = clock_();
  turns_.expire(now);

  bool type_running = false;
  std::set<LeaseKey> other_types_running;
  std::map<std::string_view, std::uint64_t> estimated_by_session;
  for (const auto& [key, admitted] : admitted_) {
    if (admitted.task_type == lease.task_type) {
      type_running = true;
    } else {
      other_types_running.insert(key);
    }
    estimated_by_session[key.first] += admitted.bytes;
  }

  if (type_running) {
    if (const std::optional<std::string> waiting = turns_.yields_to(lease.task_type)) {
      return AdmissionDecision{
          .admitted = false,
          .code = std::string(kRejectInsufficientMemory),
          .message = "memory this worker frees goes to " + *waiting +
                     " leases first, which were refused for memory and are waiting for it"};
    }
  }

  std::uint64_t pending = 0;
  for (const auto& [session, estimated] : estimated_by_session) {
    pending += pending_bytes(estimated, in_use ? in_use(session) : std::nullopt);
  }
  AdmissionDecision decision =
      decide_admission(policy_, physical_memory_bytes_, memory, pending, lease.required_bytes);
  if (decision.admitted) {
    admitted_[{std::string(lease.session), std::string(lease.lease_id)}] =
        Admitted{.task_type = std::string(lease.task_type), .bytes = lease.required_bytes};
    turns_.admitted(lease.task_type);
  } else if (decision.code == kRejectInsufficientMemory) {
    turns_.refused(lease.task_type, other_types_running, now);
  }
  return decision;
}

void AdmissionLedger::release(std::string_view session, std::string_view lease_id) {
  const std::lock_guard lock(mutex_);
  admitted_.erase({std::string(session), std::string(lease_id)});
}

void AdmissionLedger::release_session(std::string_view session) {
  const std::lock_guard lock(mutex_);
  std::erase_if(admitted_, [&](const auto& entry) { return entry.first.first == session; });
}

std::uint64_t AdmissionLedger::committed_bytes() const {
  const std::lock_guard lock(mutex_);
  std::uint64_t committed = 0;
  for (const auto& [key, admitted] : admitted_) {
    committed += admitted.bytes;
  }
  return committed;
}

bool AdmissionLedger::waiting(std::string_view task_type) const {
  const std::lock_guard lock(mutex_);
  return turns_.waiting(task_type);
}

std::uint64_t AdmissionLedger::reserve_bytes() const noexcept {
  return policy_.reserve_bytes(physical_memory_bytes_);
}

}  // namespace svp::exec::worker
