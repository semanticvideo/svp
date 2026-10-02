#include "worker_supplies.hpp"

namespace svp::builder::workers {

SuppliedSession supply_worker_session(svp::exec::FrameReader& reader,
                                      svp::exec::FrameWriter& writer,
                                      const WorkerSupplies& supplies) {
  svp::exec::worker::WorkerSessionClient client(reader, writer);
  SuppliedSession session;
  session.ack = client.hello(supplies.hello);
  client.ensure_runtime(supplies.runtime, session.stats);
  client.ensure_model_bundles(supplies.models, session.stats);
  client.send_blobs(supplies.blobs, session.stats);
  return session;
}

svp::exec::remote::RemoteSessionPreamble make_supplying_preamble(
    std::shared_ptr<const WorkerSupplies> supplies) {
  return [supplies = std::move(supplies)](svp::exec::FrameReader& reader,
                                          svp::exec::FrameWriter& writer) {
    (void)supply_worker_session(reader, writer, *supplies);
  };
}

}  // namespace svp::builder::workers
