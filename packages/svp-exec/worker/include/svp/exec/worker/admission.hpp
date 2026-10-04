#pragma once

// Memory admission (plan §3.5, §7.3): a worker accepts a lease only when the
// memory the system can hand out, minus a reserve it keeps for everything
// else on the Mac (macOS itself, a logged-in user's apps, EXO), covers the
// task's estimated peak RSS on top of what the leases it already admitted
// and not yet finished are still expected to take. Under memory pressure it
// refuses outright.
//
// The reserve scales with the Mac instead of assuming one: it is the larger
// of a floor and a fraction of physical memory.
//   * reserve_floor_bytes 2 GiB: below this macOS has no room left for the
//     window server, the file cache SVP's own decoding relies on, and the
//     user's foreground work, whatever the machine; plan §1.1 measured 5.6 to
//     9 GB available at idle on 16 GiB Macs, so the floor still leaves most
//     of that to SVP there.
//   * reserve_physical_divisor 8 (one eighth): grows the reserve with larger
//     Macs, whose other workloads (EXO model instances, plan §3.8) grow with
//     them; on a 16 GiB Mac it equals the floor.
//   * refuse_at warning: at the kernel's warning level it is already
//     compressing and swapping; admitting more work pushes it toward the
//     critical level, where it starts terminating processes.
//
// Counting running leases once. `available` (host_facts.hpp) already
// excludes the memory running tasks hold, so a running lease's estimate must
// not be subtracted again in full. Each session's leases run in that
// session's one process (session_process.hpp), so the ledger counts, per
// session,
//   pending = max(0, sum of its running leases' estimates
//                    - its process's resident memory now)
// i.e. only the part of the estimates the process has not reached yet, and
// admits a lease of `required` bytes when
//   available - reserve - sum of every session's pending >= required.
// When a session's process memory cannot be read (not started, exited, not
// readable: session_memory.hpp), its estimates count in full, as before.
// The process's memory includes what it keeps between leases (loaded models,
// allocator caches); crediting that to its running leases can understate
// what they still take when they need different models. The reserve and the
// refusal at memory pressure cover that difference.
//
// Taking turns (memory_turns.hpp): a type refused for memory makes the other
// types that run here yield the memory they free to it, for a bounded time.

#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/memory_turns.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace svp::exec::worker {

inline constexpr std::uint64_t kDefaultAdmissionReserveFloorBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kDefaultAdmissionReservePhysicalDivisor = 8;

// REJECT codes this worker sends (lease_frames.hpp LeaseRejection::code).
inline constexpr std::string_view kRejectInsufficientMemory = "insufficient_memory";
inline constexpr std::string_view kRejectMemoryPressure = "memory_pressure";

struct AdmissionPolicy {
  std::uint64_t reserve_floor_bytes = kDefaultAdmissionReserveFloorBytes;
  std::uint64_t reserve_physical_divisor = kDefaultAdmissionReservePhysicalDivisor;
  MemoryPressure refuse_at = MemoryPressure::warning;

  [[nodiscard]] std::uint64_t reserve_bytes(std::uint64_t physical_memory_bytes) const noexcept;
};

struct AdmissionDecision {
  bool admitted = false;
  // Empty when admitted; otherwise a REJECT code.
  std::string code;
  std::string message;
};

// Pure decision. `pending` is what leases already admitted and still
// running are expected to take beyond what they already use (see
// pending_bytes below; summed over sessions); `required_bytes` the new
// task's estimate (0 when its task type has none, which admits it whenever
// the reserve and pressure allow).
[[nodiscard]] AdmissionDecision decide_admission(const AdmissionPolicy& policy,
                                                 std::uint64_t physical_memory_bytes,
                                                 const MemorySnapshot& memory,
                                                 std::uint64_t pending,
                                                 std::uint64_t required_bytes);

// One session's pending bytes: its running leases' `estimated_bytes` less
// the memory its process uses now (`in_use`), never below zero; all of
// `estimated_bytes` when `in_use` is unknown.
[[nodiscard]] std::uint64_t pending_bytes(std::uint64_t estimated_bytes,
                                          std::optional<std::uint64_t> in_use) noexcept;

struct LeaseAdmission {
  std::string_view session;
  std::string_view lease_id;
  std::string_view task_type;
  // The task's estimated peak RSS (0: none).
  std::uint64_t required_bytes = 0;
};

// Memory a session's process uses now, or nullopt when unknown
// (SessionMemory::in_use).
using SessionInUse = std::function<std::optional<std::uint64_t>(std::string_view session)>;

// Thread-safe record of admitted leases, shared by every session of one
// worker agent so concurrent sessions cannot over-commit together. Leases
// are keyed by (session, lease_id): lease ids are only unique per
// coordinator run.
class AdmissionLedger {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  // Empty `clock`: steady_clock.
  AdmissionLedger(AdmissionPolicy policy, std::uint64_t physical_memory_bytes,
                  MemoryTurnPolicy turns = {}, Clock clock = {});

  // Applies the turn rule, then decides with `memory` and each session's
  // memory in use (`in_use`; empty: unknown for every session) and, when
  // admitted, records the lease.
  AdmissionDecision try_admit(const LeaseAdmission& lease, const MemorySnapshot& memory,
                              const SessionInUse& in_use = {});
  // Forgets a lease (finished, cancelled, or its session ended). Idempotent.
  void release(std::string_view session, std::string_view lease_id);
  void release_session(std::string_view session);

  // Sum of the estimates of every admitted, running lease.
  [[nodiscard]] std::uint64_t committed_bytes() const;
  [[nodiscard]] std::uint64_t reserve_bytes() const noexcept;
  [[nodiscard]] const AdmissionPolicy& policy() const noexcept { return policy_; }
  // Whether leases of `task_type` wait for their turn (memory_turns.hpp).
  [[nodiscard]] bool waiting(std::string_view task_type) const;

 private:
  struct Admitted {
    std::string task_type;
    std::uint64_t bytes = 0;
  };

  AdmissionPolicy policy_;
  std::uint64_t physical_memory_bytes_;
  Clock clock_;
  mutable std::mutex mutex_;
  MemoryTurns turns_;
  std::map<LeaseKey, Admitted, std::less<>> admitted_;
};

}  // namespace svp::exec::worker
