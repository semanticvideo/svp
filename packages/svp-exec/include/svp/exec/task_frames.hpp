#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <vector>

namespace svp::exec {

// Carriage of TaskSpec and TaskResult in frames. Only the record is defined
// here; the lease-carrying ASSIGN the scheduler sends, and HEARTBEAT, CANCEL,
// and SHUTDOWN, are in lease_frames.hpp.
//
// ASSIGN body: {"task_spec": <TaskSpec JSON>}, no payloads.
// RESULT body: {"task_result": <TaskResult JSON>}, payload i holds the bytes
// of outputs[i], so payload count, lengths, and BLAKE3 must equal the output
// ArtifactRefs.

[[nodiscard]] Frame make_assign_frame(const TaskSpec& spec);
[[nodiscard]] TaskSpec task_spec_from_assign_frame(const Frame& frame);

// Throws ExecError(invalid_value) when a payload's length or digest does not
// match its output ArtifactRef.
[[nodiscard]] Frame make_result_frame(const TaskResult& result,
                                      std::vector<FramePayload> output_payloads);
// Throws ExecError(payload_hash_mismatch) when a payload does not match its
// output ArtifactRef and (frame_malformed) for the wrong message type, body
// shape, or payload count.
[[nodiscard]] TaskResult task_result_from_result_frame(const Frame& frame);

}  // namespace svp::exec
