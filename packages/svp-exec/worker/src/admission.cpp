#include "svp/exec/worker/admission.hpp"

#include <algorithm>

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
                                   const MemorySnapshot& memory, std::uint64_t committed_bytes,
                                   std::uint64_t required_bytes) {
  if (static_cast<int>(memory.pressure) >= static_cast<int>(policy.refuse_at)) {
    return AdmissionDecision{
        .admitted = false,
        .code = std::string(kRejectMemoryPressure),
        .message = "the worker is under " + std::string(memory_pressure_name(memory.pressure)) +
                   " memory pressure"};
  }
  const std::uint64_t reserve = policy.reserve_bytes(physical_memory_bytes);
  const std::uint64_t spoken_for = reserve + committed_bytes;
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
                   " reserve, and has " + mib(committed_bytes) + " admitted to running tasks"};
  }
  return AdmissionDecision{.admitted = true, .code = {}, .message = {}};
}

AdmissionLedger::AdmissionLedger(AdmissionPolicy policy, std::uint64_t physical_memory_bytes)
    : policy_(policy), physical_memory_bytes_(physical_memory_bytes) {}

AdmissionDecision AdmissionLedger::try_admit(std::string_view session, std::string_view lease_id,
                                             std::uint64_t required_bytes,
                                             const MemorySnapshot& memory) {
  const std::lock_guard lock(mutex_);
  std::uint64_t committed = 0;
  for (const auto& [key, bytes] : admitted_) {
    committed += bytes;
  }
  AdmissionDecision decision =
      decide_admission(policy_, physical_memory_bytes_, memory, committed, required_bytes);
  if (decision.admitted) {
    admitted_[{std::string(session), std::string(lease_id)}] = required_bytes;
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
  for (const auto& [key, bytes] : admitted_) {
    committed += bytes;
  }
  return committed;
}

std::uint64_t AdmissionLedger::reserve_bytes() const noexcept {
  return policy_.reserve_bytes(physical_memory_bytes_);
}

}  // namespace svp::exec::worker
