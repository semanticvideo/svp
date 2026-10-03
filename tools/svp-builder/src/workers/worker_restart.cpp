#include "worker_restart.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_restart_wait.hpp"

namespace svp::builder::workers {

using namespace svp::exec::worker;
namespace remote = svp::exec::remote;

namespace {

std::uint64_t runtime_bytes(const CoordinatorRuntime& runtime) {
  std::uint64_t total = 0;
  for (const svp::exec::RuntimeManifestFile& file : runtime.manifest.files) {
    total += file.size_bytes;
  }
  return total;
}

}  // namespace

std::optional<WorkerHelloAck> await_worker_runtime_switch(
    const remote::PairingKey& key, const CoordinatorHello& hello,
    const CoordinatorRuntime& runtime, const WorkerHelloAck& ack,
    const svp::exec::CancellationToken* cancellation,
    const std::function<void(const std::string&)>& log) {
  const svp::exec::RuntimeRelease release{.runtime_id = runtime.runtime_id,
                                          .release_stamp = runtime.release_stamp};
  if (!worker_will_switch_to(ack, release)) {
    return std::nullopt;
  }
  const std::chrono::milliseconds deadline = worker_restart_deadline(runtime_bytes(runtime));
  if (log) {
    log(key.pairing_id + ": its service moves to runtime " +
        svp::exec::blake3_prefixed(runtime.runtime_id) + "; waiting up to " +
        std::to_string(deadline.count() / 1000) + " s for it to restart on it");
  }
  RestartWaitHooks hooks;
  hooks.probe = [&] {
    WorkerProbeResult result;
    try {
      const std::unique_ptr<WorkerConnection> connection = connect_to_worker(key);
      WorkerSessionClient client(*connection->reader, *connection->writer);
      result.ack = client.hello(hello);
      client.shutdown();
    } catch (const remote::RemoteTransportError& error) {
      result.error = error.what();
      result.permanent = error.code() == remote::RemoteErrorCode::authentication_failed;
    } catch (const WorkerError& error) {
      result.error = error.what();
      result.permanent = error.code() == WorkerErrorCode::refused;
    } catch (const std::exception& error) {
      // A session the restarting service closed unanswered.
      result.error = error.what();
    }
    return result;
  };
  if (cancellation != nullptr) {
    hooks.cancelled = [cancellation] { return cancellation->requested(); };
  }
  const RestartWaitOutcome outcome = wait_for_worker_restart(release, deadline, hooks);
  if (log) {
    const bool switched = outcome.ack && outcome.ack->agent_runtime_id == runtime.runtime_id;
    log(key.pairing_id + ": " +
        (switched ? std::string("restarted on the new runtime")
                  : std::string(restart_wait_end_name(outcome.end)) +
                        (outcome.last_error.empty() ? "" : " (" + outcome.last_error + ")")) +
        " after " + std::to_string(outcome.attempts) + " attempt(s)");
  }
  if (!outcome.ack || !outcome.ack->accepted()) {
    return std::nullopt;
  }
  return outcome.ack;
}

}  // namespace svp::builder::workers
