#pragma once

#include "svp/exec/executor.hpp"
#include "svp/exec/frame_limits.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/loopback_executor.hpp"
#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/route_policy.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec::remote {

// How a session rides out a worker that is briefly unreachable, for example
// a worker daemon that crashed and is being restarted:
//   * reconnect_window 15 s: launchd respawns a KeepAlive daemon no sooner
//     than its ThrottleInterval, 10 s by default, and the new process must
//     then re-register with Bonjour (about 1 s, remote_listener.hpp). 15 s
//     covers both and stays below the default 30 s lease floor
//     (lease_policy.hpp), so leases queued on a reconnecting session are
//     never expired by the scheduler while it is still trying.
//   * reconnect_pause 500 ms between attempts that failed quickly (a refused
//     connection to a stale Bonjour record of the dead process): one
//     discovery settle period (route_policy.hpp), long enough for mDNS
//     caches to see the old record withdrawn and the new one announced.
// Authentication failures are never retried: the peer does not hold the
// secret, and waiting will not change that.
inline constexpr std::chrono::milliseconds kDefaultReconnectWindow{15'000};
inline constexpr std::chrono::milliseconds kDefaultReconnectPause{500};

// Runs on a session's own thread once its connection is authenticated and
// before any ASSIGN is sent: the worker handshake (HELLO / HELLO_ACK, runtime
// and model transfer, plan §4.3) belongs here. It reads the worker's replies
// from `reader` and writes requests to `writer`; frames it leaves unread stay
// in `reader` for the lease pump. Throwing ends the session: its queued
// leases fail with executor_lost and the exception text as the reason.
using RemoteSessionPreamble = std::function<void(FrameReader& reader, FrameWriter& writer)>;

struct RemoteExecutorOptions {
  // Stable and unique within a scheduler run. Required.
  std::string executor_id;
  RemoteConnectorOptions connector;
  // Leases the remote worker runs at once. Required (>= 1) and deliberately
  // without a default: it is the worker's advertised or measured capacity
  // (plan §3.5), never a count baked into code.
  std::size_t slots = 0;
  // After SHUTDOWN and closing its output, how long a session may take to
  // end before the connection is torn down (plan §4.4 grace period).
  std::chrono::milliseconds shutdown_grace = kDefaultWorkerShutdownGrace;
  std::chrono::milliseconds reconnect_window = kDefaultReconnectWindow;
  std::chrono::milliseconds reconnect_pause = kDefaultReconnectPause;
  FrameLimits frame_limits{};
  // Empty: leases are sent as soon as the connection opens (a bare worker
  // loop, such as svp-exec-test-worker --listen).
  RemoteSessionPreamble session_preamble;
};

// Runs tasks on a paired worker over the remote transport, speaking the same
// framed protocol as LoopbackExecutor (plan §3.1, §4.3) and applying the
// same rules through WorkerSessionLeases / pump_worker_frames:
//
//   * The first assign() opens a session: discovery by pairing id, route
//     selection, the TLS handshake, and the session preamble run on the
//     session's own thread, so the scheduler thread never waits on the
//     network; ASSIGN frames issued meanwhile are queued and sent once the
//     preamble has finished.
//   * Results are verified (schema, output_digest, every payload's length
//     and BLAKE3) before they reach the scheduler. Bytes that fail any check,
//     or a result for a lease this executor did not issue, end the session:
//     outstanding leases fail with invalid_result.
//   * A session that cannot connect keeps rediscovering the worker by
//     pairing id for reconnect_window, so a restarted worker on a new port or
//     address is found again; after that, or on an authentication failure,
//     its leases fail with executor_lost.
//   * A connection that ends (the worker process died, the network went
//     away, the peer reset) fails outstanding leases with executor_lost; the
//     next assign() opens a fresh session.
//   * lease_expired() means the session stopped heartbeating: its
//     connection is torn down and its other leases fail as lost.
//   * loss_quarantine() is after_repeated_losses: a worker that keeps
//     dropping sessions without completing work in between is routed around
//     (plan §4.4); one dropped session is one loss event however many of its
//     slots were busy.
class RemoteExecutor final : public Executor {
 public:
  explicit RemoteExecutor(RemoteExecutorOptions options);
  ~RemoteExecutor() override;
  RemoteExecutor(const RemoteExecutor&) = delete;
  RemoteExecutor& operator=(const RemoteExecutor&) = delete;

  [[nodiscard]] std::string_view id() const override;
  [[nodiscard]] std::size_t slots() const override;
  [[nodiscard]] LossQuarantine loss_quarantine() const override;
  void start(ExecutorEvents& events) override;
  void assign(const TaskSpec& spec, const Lease& lease) override;
  void cancel(std::string_view lease_id) override;
  void lease_expired(std::string_view lease_id) override;
  void stop() override;

  // Sessions whose connection opened so far (tests observe reconnects).
  [[nodiscard]] std::size_t connections_opened() const;
  // Route of the most recently opened connection.
  [[nodiscard]] std::optional<RouteChoice> last_route() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace svp::exec::remote
