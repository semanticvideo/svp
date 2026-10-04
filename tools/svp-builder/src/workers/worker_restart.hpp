#pragma once

// Waiting for a worker whose service is about to restart on a newer runtime
// (worker_restart_wait.hpp), on every coordinator path: after a session whose
// HELLO_ACK showed a switch is due (the worker's reported pending switch,
// whoever installed that runtime, or this coordinator's runtime it just
// installed), the next session first waits for the worker to come back.
//   - one-off sessions (workers sync, workers fleet pair, the calibration
//     command, build --distributed prepare): await_worker_runtime_switch
//     right after the session;
//   - repeated sessions (a build's task and calibration sessions, a batch's
//     whole-video jobs): a RuntimeSwitchWatch per worker, filled by each
//     session and checked before the next (with_supplied_sessions in
//     worker_supplies.hpp, paired_video_builder.hpp).

#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/runtime_source.hpp"
#include "svp/exec/worker/worker_restart_wait.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace svp::builder::workers {

// `runtime` as an offer (its release and file bytes).
[[nodiscard]] svp::exec::worker::RuntimeOffer runtime_offer(
    const svp::exec::worker::CoordinatorRuntime& runtime);

// When `ack` shows a switch is due (expected_switch with this coordinator's
// `runtime`, now installed there), waits within worker_restart_deadline of
// the runtime switched to and returns the HELLO_ACK the worker then sent;
// otherwise returns nullopt at once. Progress goes to `log` when given;
// `cancelled` ends the wait early. Never throws: a worker that does not come
// back is the next session's problem, reported there as before.
[[nodiscard]] std::optional<svp::exec::worker::WorkerHelloAck> await_worker_runtime_switch(
    const svp::exec::remote::PairingKey& key, const svp::exec::worker::CoordinatorHello& hello,
    const svp::exec::worker::CoordinatorRuntime& runtime,
    const svp::exec::worker::WorkerHelloAck& ack, const std::function<bool()>& cancelled,
    const std::function<void(const std::string&)>& log);

// The watch shared by every repeated session of this process to the worker
// of `pairing_id` offered `runtime`.
[[nodiscard]] std::shared_ptr<svp::exec::worker::RuntimeSwitchWatch> runtime_switch_watch(
    const std::string& pairing_id, const svp::exec::worker::CoordinatorRuntime& runtime);

}  // namespace svp::builder::workers
