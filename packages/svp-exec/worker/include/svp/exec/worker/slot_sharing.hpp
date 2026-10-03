#pragma once

// Slot sharing across coordinators (M6): a worker that serves several
// coordinators at once shares each task type's measured slots among them, on
// top of memory admission (admission.hpp), so no coordinator starves another
// and the Mac never runs more of a type at once than one coordinator alone
// would send it.
//
// Each session declares, in HELLO `capacity`, the slots its coordinator
// measured for this worker per task type (hello_messages.hpp). For a lease of
// type T from coordinator C declaring S slots:
//   * Uncontended: nobody but C holds or waits for T (no other coordinator's
//     lease, no task of T running locally on this Mac). The lease is
//     admitted: a coordinator alone uses a worker exactly as before M6.
//   * Contended: the lease is admitted only when fewer than S leases of T are
//     running here (every coordinator's and this Mac's own), and no other
//     coordinator waiting for T comes first. Waiting coordinators are served
//     fewest-held first, then in the order they started waiting, so a
//     coordinator whose lease just finished cannot take the freed slot ahead
//     of one that was turned away.
//   * Otherwise it is refused with REJECT kRejectInsufficientSlots, and C
//     waits for T. The coordinator's scheduler backs off that worker and
//     runs the task elsewhere (scheduler.hpp); it asks again after its
//     rejection backoff.
// A coordinator stops waiting once a lease of T is admitted, or when it has
// not asked again within the contention window (its tasks went elsewhere or
// ran out), so a slot is never held back for a coordinator that left.
//
// This Mac's own build (local_load.hpp) counts as a holder that never waits:
// remote leases only take slots it is not using, and it is never refused
// anything (it does not ask).
//
// Leases from sessions that declare no slots for their type (pairing,
// calibration, 1.0 coordinators) are admitted without this check, as before,
// and count as held while they run.

#include "svp/exec/scheduler_policy.hpp"
#include "svp/exec/worker/admission.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace svp::exec::worker {

inline constexpr std::string_view kRejectInsufficientSlots = "insufficient_slots";

// A coordinator turned away retries every rejection backoff while it still
// has tasks of the type (scheduler_policy.hpp, 2 s by default). Five missed
// retries in a row means it no longer asks: its tasks went elsewhere or ran
// out, and the slot it was waiting for goes to whoever asks next.
inline constexpr std::uint64_t kSlotContentionWindowBackoffs = 5;
inline constexpr std::chrono::milliseconds kDefaultSlotContentionWindow =
    kDefaultRejectionBackoff * kSlotContentionWindowBackoffs;

// Running task counts per task type.
using TaskTypeCounts = std::map<std::string, std::uint64_t, std::less<>>;

struct SlotRequest {
  // Who the lease is for: the pairing the session came in on.
  std::string coordinator;
  std::string session;
  std::string lease_id;
  std::string task_type;
  // The session's declared slots for task_type; 0 when it declared none.
  std::uint64_t declared_slots = 0;
};

class SlotSharing {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  // The tasks of each type this Mac's own build runs right now.
  using LocalLoad = std::function<TaskTypeCounts()>;

  // Empty `local_load`: nothing runs locally. Empty `clock`: steady_clock.
  explicit SlotSharing(std::chrono::milliseconds contention_window = kDefaultSlotContentionWindow,
                       LocalLoad local_load = {}, Clock clock = {});

  // Decides, and records the lease when admitted. Thread-safe.
  AdmissionDecision try_take(const SlotRequest& request);
  // Forgets a lease (finished, cancelled, refused by memory). Idempotent.
  void release(std::string_view session, std::string_view lease_id);
  void release_session(std::string_view session);

  // Leases of `task_type` held here, every coordinator together.
  [[nodiscard]] std::uint64_t held(std::string_view task_type) const;

 private:
  struct Held {
    std::string coordinator;
    std::string task_type;
  };
  struct Waiting {
    std::uint64_t ticket = 0;
    std::chrono::steady_clock::time_point last_asked;
  };

  void expire_waiting(std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::uint64_t held_by(std::string_view coordinator,
                                      std::string_view task_type) const;
  void wait(const SlotRequest& request, std::chrono::steady_clock::time_point now);

  std::chrono::milliseconds contention_window_;
  LocalLoad local_load_;
  Clock clock_;
  mutable std::mutex mutex_;
  // (session, lease_id) -> holder.
  std::map<std::pair<std::string, std::string>, Held, std::less<>> held_;
  // task_type -> coordinator -> waiting since.
  std::map<std::string, std::map<std::string, Waiting, std::less<>>, std::less<>> waiting_;
  std::uint64_t next_ticket_ = 0;
};

}  // namespace svp::exec::worker
