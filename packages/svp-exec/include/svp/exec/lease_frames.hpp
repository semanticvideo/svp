#pragma once

#include "svp/exec/executor.hpp"
#include "svp/exec/frame.hpp"
#include "svp/exec/task_spec.hpp"

#include <string>
#include <string_view>

namespace svp::exec {

// Lease-carrying protocol messages (plan §4.3). RESULT frames use
// make_result_frame / task_result_from_result_frame unchanged; the scheduler
// matches a result to its lease by (task_id, attempt).
//
// ASSIGN    body {"lease":{"attempt","expires_in_ms","heartbeat_interval_ms",
//                          "lease_id"},
//                 "task_spec":{...}}                     no payloads
// HEARTBEAT body {"lease_id"}                            no payloads
// CANCEL    body {"lease_id"}                            no payloads
// SHUTDOWN  body {}                                      no payloads
// REJECT    body {"code","lease_id","message"}            no payloads
//
// lease_id is a record identifier ([A-Za-z0-9._-]); attempt, expires_in_ms,
// and heartbeat_interval_ms are integers >= 1. Parsers reject unknown fields
// and throw ExecError(frame_malformed) for the wrong type or payloads.

struct LeasedAssignment {
  TaskSpec spec;
  Lease lease;
};

[[nodiscard]] Frame make_leased_assign_frame(const TaskSpec& spec, const Lease& lease);
[[nodiscard]] LeasedAssignment leased_assignment_from_frame(const Frame& frame);

[[nodiscard]] Frame make_heartbeat_frame(std::string_view lease_id);
[[nodiscard]] std::string lease_id_from_heartbeat_frame(const Frame& frame);

[[nodiscard]] Frame make_cancel_frame(std::string_view lease_id);
[[nodiscard]] std::string lease_id_from_cancel_frame(const Frame& frame);

[[nodiscard]] Frame make_shutdown_frame();

// A worker's refusal to run one lease it was assigned (plan §4.3 ACCEPT /
// REJECT: "admission decision (insufficient_memory, model_missing,
// os_mismatch, ...)"). `code` is a lower-case [a-z0-9_] identifier naming the
// reason; `message` is for people. The lease never ran and the worker is
// healthy, so the coordinator reports it with ExecutorEvents::attempt_rejected:
// not a failed attempt, not a loss (scheduler.hpp). Protocol errors are ERROR
// frames and end the session instead.
struct LeaseRejection {
  std::string lease_id;
  std::string code;
  std::string message;

  bool operator==(const LeaseRejection&) const = default;
};

[[nodiscard]] Frame make_reject_frame(const LeaseRejection& rejection);
[[nodiscard]] LeaseRejection lease_rejection_from_frame(const Frame& frame);

}  // namespace svp::exec
