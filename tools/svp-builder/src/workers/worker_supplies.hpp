#pragma once

// What a coordinator brings every worker session before tasks flow (plan
// §4.1, §4.3): HELLO with this build's runtime, macOS, and thread plan (the
// worker refuses mismatches), then the runtime, the model bundles the tasks
// load, and the blobs they read (a build's source, a calibration clip), each
// sent only when the worker lacks it and verified there by BLAKE3.

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/worker/blob_source.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/runtime_source.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace svp::builder::workers {

struct WorkerSupplies {
  svp::exec::worker::CoordinatorHello hello;
  svp::exec::worker::CoordinatorRuntime runtime;
  std::vector<svp::exec::worker::ModelBundleSource> models;
  std::vector<svp::exec::worker::BlobSource> blobs;
};

struct SuppliedSession {
  svp::exec::worker::WorkerHelloAck ack;
  svp::exec::worker::TransferStats stats;
};

// On an open session: HELLO, then the runtime, models, and blobs. Throws
// WorkerError (refused, protocol, verification, io).
SuppliedSession supply_worker_session(svp::exec::FrameReader& reader,
                                      svp::exec::FrameWriter& writer,
                                      const WorkerSupplies& supplies);

// `supplies` whose HELLO declares `slots` of `task_type` (M6 slot sharing,
// slot_sharing.hpp): what a session restricted to that type tells the worker
// it measured, so a worker serving several coordinators shares that type's
// slots among them.
[[nodiscard]] std::shared_ptr<const WorkerSupplies> declaring_capacity(
    const std::shared_ptr<const WorkerSupplies>& supplies, std::string_view task_type,
    std::size_t slots);

// The same as a RemoteExecutor session preamble, so every session (and every
// reconnect) is checked and supplied before its leases are sent.
[[nodiscard]] svp::exec::remote::RemoteSessionPreamble make_supplying_preamble(
    std::shared_ptr<const WorkerSupplies> supplies);

}  // namespace svp::builder::workers
