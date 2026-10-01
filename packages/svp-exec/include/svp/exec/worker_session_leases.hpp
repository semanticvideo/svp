#pragma once

#include "svp/exec/executor.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/task_result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// Coordinator-side record of the leases one worker session holds. Shared by
// every executor that talks to a worker over the framed protocol (loopback
// child process, remote connection) so they apply one set of rules. Not
// thread-safe: the owning executor guards it with its own mutex.
class WorkerSessionLeases {
 public:
  void add(std::string lease_id, std::string task_id, std::uint64_t attempt);
  [[nodiscard]] bool contains(std::string_view lease_id) const;

  // A cancelled lease stays known so a result already in flight is recognised
  // and ignored rather than treated as a result nobody asked for. Returns
  // false when the lease is unknown.
  bool mark_cancelled(std::string_view lease_id);

  // True when the lease is known and not cancelled.
  [[nodiscard]] bool is_live(std::string_view lease_id) const;

  // Removes the lease a RESULT belongs to (matched by task_id and attempt)
  // and returns it, or nullopt when that lease was cancelled. Throws
  // ExecError(frame_malformed) when no lease of this session matches.
  std::optional<std::string> claim_result(const TaskResult& result);

  // Removes every lease and returns the ones not cancelled, in lease_id
  // order: the leases a lost session leaves orphaned.
  std::vector<std::string> take_live();

 private:
  struct Outstanding {
    std::string task_id;
    std::uint64_t attempt = 0;
    bool cancelled = false;
  };
  std::map<std::string, Outstanding, std::less<>> leases_;
};

// How a worker session's frame stream ended, seen from the coordinator.
struct WorkerSessionEnd {
  AttemptFailureKind failure = AttemptFailureKind::executor_lost;
  std::string reason;
};

// Reads worker frames until the stream ends and reports what they mean
// (plan §4.4):
//   * HEARTBEAT for a live lease renews it (events.lease_heartbeat).
//   * RESULT is decoded with task_result_from_result_frame, which verifies
//     the schema, output_digest, and every payload's length and BLAKE3, and
//     is claimed against `leases`; a verified result for a live lease is
//     reported with events.attempt_finished.
//   * ERROR from the worker ends the session as executor_lost.
//   * Bytes that fail any check, a result for a lease this session did not
//     issue, or any other frame end the session as invalid_result.
//   * End of stream, including one that ends inside a frame (a worker that
//     died mid-write), ends it as executor_lost with `end_of_stream_reason`.
// `mutex` guards `leases`; events are delivered without it held.
[[nodiscard]] WorkerSessionEnd pump_worker_frames(FrameReader& reader, std::mutex& mutex,
                                                  WorkerSessionLeases& leases,
                                                  ExecutorEvents& events,
                                                  std::string_view end_of_stream_reason);

}  // namespace svp::exec
