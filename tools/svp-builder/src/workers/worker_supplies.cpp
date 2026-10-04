#include "worker_supplies.hpp"

#include "worker_restart.hpp"

#include "svp/exec/worker/pairing_proof.hpp"
#include "svp/exec/worker/worker_id_book.hpp"

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

svp::exec::remote::RemoteExecutorOptions with_supplied_sessions(
    svp::exec::remote::RemoteExecutorOptions options,
    std::shared_ptr<const WorkerSupplies> supplies) {
  const svp::exec::remote::PairingKey key = options.connector.pairing;
  const std::shared_ptr<svp::exec::worker::RuntimeSwitchWatch> watch =
      runtime_switch_watch(key.pairing_id, supplies->runtime);
  options.connector.worker_id =
      svp::exec::worker::default_worker_id_book().worker_id_of(key.pairing_id);
  options.session_preamble = [supplies, watch, key](svp::exec::FrameReader& reader,
                                                    svp::exec::FrameWriter& writer,
                                                    svp::exec::remote::RemoteStream& stream) {
    // HELLO carries this pairing's proof; HELLO_ACK's worker id is learned.
    svp::exec::worker::ProvingFrameWriter proving(writer,
                                                  svp::exec::worker::prove_pairing(stream, key));
    svp::exec::worker::AckObservingFrameReader learning(
        reader, [&key](const svp::exec::worker::WorkerHelloAck& ack) {
          svp::exec::worker::default_worker_id_book().learn(key.pairing_id, ack.worker_id);
        });
    watch->observed(supply_worker_session(learning, proving, *supplies).ack);
  };
  options.before_connect = [supplies, watch, key](const std::function<bool()>& stop_requested) {
    if (const std::optional<svp::exec::worker::WorkerHelloAck> due = watch->take_due()) {
      watch->after_wait(await_worker_runtime_switch(key, supplies->hello, supplies->runtime,
                                                    *due, stop_requested, {}),
                        *due);
    }
  };
  return options;
}

}  // namespace svp::builder::workers
