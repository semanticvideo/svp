#pragma once

// Memory turns: keeps one task type from taking every byte of memory a
// worker frees while another type waits for enough of it (admission.hpp).
//
// Without turns, a worker running a stream of small leases of type U (OCR
// frame batches) re-admits U the moment one of them ends, because U's
// executor is offered work again as soon as its attempt finishes, while a
// lease of a larger type T (an ASR chunk batch) that was refused for memory
// waits out its rejection backoff (scheduler_policy.hpp) and finds the
// memory taken again. T then never runs on that worker while U has work.
//
// The rule, applied by AdmissionLedger to every lease of this worker
// (all coordinators together, since memory is the Mac's):
//   * Wait: a lease of type T refused for insufficient memory, while leases
//     of other types run here, makes T wait. The wait remembers which leases
//     of other types were running then: those are the leases whose memory T
//     is waiting for.
//   * Yield: while T waits, a lease of another type U is refused (REJECT
//     insufficient_memory, so U backs off and runs elsewhere) when U already
//     holds a lease here. A type with no lease here is not held back: it
//     cannot be what keeps T out. When several types wait, they are served in
//     the order they started waiting; a waiting type yields only to types that
//     started waiting before it, so two waiting types never block each other.
//   * End: the wait ends when
//       - a lease of T is admitted (T got its turn);
//       - T has not asked again within the idle window (its tasks went
//         elsewhere or ran out), as in slot_sharing.hpp;
//       - T is refused again after every lease it was waiting for has ended
//         (fruitless: what remains in use is not the other types' doing); or
//       - the wait has lasted the max wait (fruitless; a backstop for leases
//         it waits for that never end).
//   * Rest: after a fruitless wait, T does not start waiting again for as
//     long as that wait held the other types back, so a type that cannot fit
//     on this worker at all takes no more than an equal share of its time.
//     T is still admitted whenever its lease fits.
// No type starves: the waiting type gets the memory the others free, and the
// others are held back for a bounded time only.

#include "svp/exec/lease_policy.hpp"
#include "svp/exec/scheduler_policy.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace svp::exec::worker {

// A coordinator refused for memory asks again every rejection backoff while
// it has tasks of the type (scheduler_policy.hpp, 2 s by default). Five
// missed asks in a row means it no longer asks: its tasks went elsewhere or
// ran out, so the others stop yielding to it. Same reasoning and count as
// kSlotContentionWindowBackoffs (slot_sharing.hpp).
inline constexpr std::uint64_t kMemoryTurnIdleBackoffs = 5;
inline constexpr std::chrono::milliseconds kDefaultMemoryTurnIdleWindow =
    kDefaultRejectionBackoff * kMemoryTurnIdleBackoffs;

// Backstop on one wait. A wait normally ends when the leases it waits for
// end, and every one of those ends by its attempt deadline at the latest
// (lease_policy.hpp: the scheduler cancels it, the worker releases it). The
// default attempt deadline floor is the longest a default-policy attempt is
// expected to hold memory, so no wait outlasts it even if a release is lost.
inline constexpr std::chrono::milliseconds kDefaultMemoryTurnMaxWait =
    kDefaultAttemptDeadlineFloor;

struct MemoryTurnPolicy {
  std::chrono::milliseconds idle_window = kDefaultMemoryTurnIdleWindow;
  std::chrono::milliseconds max_wait = kDefaultMemoryTurnMaxWait;
};

// (session, lease_id), as AdmissionLedger keys leases.
using LeaseKey = std::pair<std::string, std::string>;

// Turn state. Not thread-safe: AdmissionLedger calls it under its own lock,
// so a decision and the state it reads are one atomic step.
class MemoryTurns {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  explicit MemoryTurns(MemoryTurnPolicy policy = {});

  // Ends waits whose idle window or max wait passed, and rests that are over.
  void expire(TimePoint now);

  // The waiting type a lease of `task_type` must yield to, if any. The caller
  // asks only for types that already hold a lease here.
  [[nodiscard]] std::optional<std::string> yields_to(std::string_view task_type) const;

  // A lease of `task_type` was refused for insufficient memory while
  // `other_types_running` (leases of other types) ran here.
  void refused(std::string_view task_type, const std::set<LeaseKey>& other_types_running,
               TimePoint now);

  // A lease of `task_type` was admitted: its wait, if any, is over.
  void admitted(std::string_view task_type);

  [[nodiscard]] bool waiting(std::string_view task_type) const;
  [[nodiscard]] bool resting(std::string_view task_type, TimePoint now) const;

 private:
  struct Wait {
    std::uint64_t ticket = 0;
    TimePoint since;
    TimePoint last_asked;
    // Leases of other types running when the wait began.
    std::set<LeaseKey> waiting_for;
  };

  void end_fruitless(std::map<std::string, Wait, std::less<>>::iterator wait, TimePoint now);

  MemoryTurnPolicy policy_;
  std::map<std::string, Wait, std::less<>> waits_;
  // task_type -> end of its rest.
  std::map<std::string, TimePoint, std::less<>> rests_;
  std::uint64_t next_ticket_ = 0;
};

}  // namespace svp::exec::worker
