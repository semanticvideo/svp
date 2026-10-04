#include "worker_restart.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <map>
#include <mutex>

namespace svp::builder::workers {

using namespace svp::exec::worker;
namespace remote = svp::exec::remote;

RuntimeOffer runtime_offer(const CoordinatorRuntime& runtime) {
  std::uint64_t bytes = 0;
  for (const svp::exec::RuntimeManifestFile& file : runtime.manifest.files) {
    bytes += file.size_bytes;
  }
  return RuntimeOffer{.release = {.runtime_id = runtime.runtime_id,
                                  .release_stamp = runtime.release_stamp},
                      .bytes = bytes};
}

namespace {

std::optional<WorkerHelloAck> await_unguarded(
    const remote::PairingKey& key, const CoordinatorHello& hello,
    const CoordinatorRuntime& runtime, const WorkerHelloAck& ack,
    const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& log) {
  const RuntimeOffer offer = runtime_offer(runtime);
  const std::optional<RuntimeOffer> target = expected_switch(ack, offer);
  if (!target) {
    return std::nullopt;
  }
  const std::chrono::milliseconds deadline = worker_restart_deadline(target->bytes);
  if (log) {
    log(key.pairing_id + ": its service moves to runtime " +
        svp::exec::blake3_prefixed(target->release.runtime_id) + "; waiting up to " +
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
  hooks.cancelled = cancelled;
  const RestartWaitOutcome outcome =
      wait_for_worker_restart(std::optional<RuntimeOffer>(offer), deadline, hooks);
  if (log) {
    const bool switched =
        outcome.ack && outcome.ack->agent_runtime_id == target->release.runtime_id;
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

}  // namespace

std::optional<WorkerHelloAck> await_worker_runtime_switch(
    const remote::PairingKey& key, const CoordinatorHello& hello,
    const CoordinatorRuntime& runtime, const WorkerHelloAck& ack,
    const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& log) {
  try {
    return await_unguarded(key, hello, runtime, ack, cancelled, log);
  } catch (const std::exception& error) {
    // Documented never to throw: every caller goes on without the wait.
    if (log) {
      try {
        log(key.pairing_id + ": restart wait failed (" + error.what() + ")");
      } catch (const std::exception&) {
      }
    }
    return std::nullopt;
  }
}

std::shared_ptr<RuntimeSwitchWatch> runtime_switch_watch(const std::string& pairing_id,
                                                         const CoordinatorRuntime& runtime) {
  static std::mutex mutex;
  static std::map<std::pair<std::string, svp::exec::Blake3Digest>,
                  std::shared_ptr<RuntimeSwitchWatch>>
      watches;
  const std::lock_guard lock(mutex);
  std::shared_ptr<RuntimeSwitchWatch>& watch = watches[{pairing_id, runtime.runtime_id}];
  if (!watch) {
    watch = std::make_shared<RuntimeSwitchWatch>(runtime_offer(runtime));
  }
  return watch;
}

}  // namespace svp::builder::workers
