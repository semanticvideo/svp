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

std::shared_ptr<const WorkerSupplies> declaring_capacity(
    const std::shared_ptr<const WorkerSupplies>& supplies, std::string_view task_type,
    std::size_t slots) {
  auto declaring = std::make_shared<WorkerSupplies>(*supplies);
  declaring->hello.capacity = {{std::string(task_type), slots}};
  return declaring;
}

svp::exec::remote::RemoteSessionPreamble make_supplying_preamble(
    std::shared_ptr<const WorkerSupplies> supplies) {
  return [supplies = std::move(supplies)](svp::exec::FrameReader& reader,
                                          svp::exec::FrameWriter& writer) {
    (void)supply_worker_session(reader, writer, *supplies);
  };
}

}  // namespace svp::builder::workers
