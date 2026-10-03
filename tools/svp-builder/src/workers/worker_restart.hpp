#pragma once

// After a session that left a newer runtime on a self-updating worker, wait
// for the worker to restart on it before the next session
// (worker_restart_wait.hpp), so `workers sync`, `workers fleet pair`, and
// `build --distributed` keep the worker they just updated.

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/runtime_source.hpp"

#include <functional>
#include <optional>
#include <string>

namespace svp::builder::workers {

// When the worker that answered `ack` will switch to `runtime` (now
// installed there), waits for it within worker_restart_deadline and returns
// the HELLO_ACK it then sent (on the new runtime when it switched);
// otherwise returns nullopt at once. Progress goes to `log` when given.
// Never throws: a worker that does not come back is the next session's
// problem, reported there as before.
[[nodiscard]] std::optional<svp::exec::worker::WorkerHelloAck> await_worker_runtime_switch(
    const svp::exec::remote::PairingKey& key, const svp::exec::worker::CoordinatorHello& hello,
    const svp::exec::worker::CoordinatorRuntime& runtime,
    const svp::exec::worker::WorkerHelloAck& ack,
    const svp::exec::CancellationToken* cancellation,
    const std::function<void(const std::string&)>& log);

}  // namespace svp::builder::workers
