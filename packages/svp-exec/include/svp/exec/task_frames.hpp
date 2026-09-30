#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/task_result.hpp"

#include <vector>

namespace svp::exec {

// Carriage of TaskResult in RESULT frames. ASSIGN always carries a lease
// (plan §4.3: TaskSpec plus lease), so it is built and parsed only by
// make_leased_assign_frame / leased_assignment_from_frame in lease_frames.hpp,
// together with HEARTBEAT, CANCEL, and SHUTDOWN. There is no lease-less ASSIGN.
//
// RESULT body: {"task_result": <TaskResult JSON>}, payload i holds the bytes
// of outputs[i], so payload count, lengths, and BLAKE3 must equal the output
// ArtifactRefs.

// Throws ExecError(invalid_value) when a payload's length or digest does not
// match its output ArtifactRef.
[[nodiscard]] Frame make_result_frame(const TaskResult& result,
                                      std::vector<FramePayload> output_payloads);
// Throws ExecError(payload_hash_mismatch) when a payload does not match its
// output ArtifactRef and (frame_malformed) for the wrong message type, body
// shape, or payload count.
[[nodiscard]] TaskResult task_result_from_result_frame(const Frame& frame);

}  // namespace svp::exec
