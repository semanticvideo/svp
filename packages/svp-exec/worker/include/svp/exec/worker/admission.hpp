#pragma once

// Memory admission (plan §3.5, §7.3): a worker accepts a lease only when the
// memory the system can hand out, minus a reserve it keeps for everything
// else on the Mac (macOS itself, a logged-in user's apps, EXO), covers the
// task's estimated peak RSS on top of every lease it has already admitted
// and not yet finished. Under memory pressure it refuses outright.
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
// Estimates already admitted are counted in full even though the tasks may
// not have reached their peak yet: double counting what a running task has
// already allocated only makes admission more conservative, never less.

#include "svp/exec/worker/host_facts.hpp"

#include <cstdint>
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

// Pure decision. `committed_bytes` is the estimated peak RSS of leases
// already admitted and still running; `required_bytes` the new task's
// estimate (0 when its task type has none, which admits it whenever the
// reserve and pressure allow).
[[nodiscard]] AdmissionDecision decide_admission(const AdmissionPolicy& policy,
                                                 std::uint64_t physical_memory_bytes,
                                                 const MemorySnapshot& memory,
                                                 std::uint64_t committed_bytes,
                                                 std::uint64_t required_bytes);

// Thread-safe record of admitted leases, shared by every session of one
// worker agent so concurrent sessions cannot over-commit together. Leases
// are keyed by (session, lease_id): lease ids are only unique per
// coordinator run.
class AdmissionLedger {
 public:
  AdmissionLedger(AdmissionPolicy policy, std::uint64_t physical_memory_bytes);

  // Decides with `memory` and, when admitted, records the lease.
  AdmissionDecision try_admit(std::string_view session, std::string_view lease_id,
                              std::uint64_t required_bytes, const MemorySnapshot& memory);
  // Forgets a lease (finished, cancelled, or its session ended). Idempotent.
  void release(std::string_view session, std::string_view lease_id);
  void release_session(std::string_view session);

  [[nodiscard]] std::uint64_t committed_bytes() const;
  [[nodiscard]] std::uint64_t reserve_bytes() const noexcept;
  [[nodiscard]] const AdmissionPolicy& policy() const noexcept { return policy_; }

 private:
  AdmissionPolicy policy_;
  std::uint64_t physical_memory_bytes_;
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, std::uint64_t, std::less<>> admitted_;
};

}  // namespace svp::exec::worker
