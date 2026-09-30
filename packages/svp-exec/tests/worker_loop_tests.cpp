#include "scheduler_test_support.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/task_frames.hpp"
#include "svp/exec/worker_loop.hpp"

#include <future>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

// Long enough that at least one kTestHeartbeatInterval passes while it runs.
constexpr std::uint64_t kTaskMs = 5 * static_cast<std::uint64_t>(kTestHeartbeatInterval.count());

struct SocketPair {
  int coordinator = -1;
  int worker = -1;
  SocketPair() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    coordinator = fds[0];
    worker = fds[1];
  }
  ~SocketPair() {
    ::close(coordinator);
    if (worker >= 0) {
      ::close(worker);
    }
  }
  SocketPair(const SocketPair&) = delete;
  SocketPair& operator=(const SocketPair&) = delete;
};

Lease lease(std::string lease_id, std::uint64_t attempt) {
  return Lease{.lease_id = std::move(lease_id),
               .attempt = attempt,
               .duration = test_lease_length(),
               .heartbeat_interval = kTestHeartbeatInterval};
}

TaskSpec sleeping_spec() {
  return make_toy_node(ToyTask{.task_id = "task.toy.loop",
                               .seed = 5,
                               .order_key = {.lane = "alpha", .ordinals = {0}},
                               .sleep_ms = kTaskMs})
      .spec;
}

void test_assign_heartbeat_result() {
  SocketPair sockets;
  ToyRuntime runtime;
  auto loop = std::async(std::launch::async, [&] {
    return run_worker_loop(sockets.worker, sockets.worker, runtime.registry, runtime.store,
                           WorkerLoopOptions{.worker_session_id = "ws_loop"});
  });
  FdFrameWriter writer(sockets.coordinator);
  FdFrameReader reader(sockets.coordinator);

  writer.write(make_leased_assign_frame(sleeping_spec(), lease("lease_1", 3)));
  std::size_t heartbeats = 0;
  std::optional<Frame> result;
  while (!result) {
    std::optional<Frame> frame = reader.read();
    expect(frame.has_value(), "worker closed early");
    if (frame->type == MessageType::heartbeat) {
      expect_equal(lease_id_from_heartbeat_frame(*frame), "lease_1", "heartbeat lease");
      ++heartbeats;
    } else {
      result = std::move(frame);
    }
  }
  expect(heartbeats >= 1, "heartbeats while the task runs");
  const TaskResult decoded = task_result_from_result_frame(*result);
  expect(decoded.attempt == 3, "result carries the lease attempt");
  expect_equal(decoded.execution.worker_session_id, "ws_loop", "worker session stamped");
  expect_equal(to_text(result->payloads.front()), toy_expected_output("task.toy.loop", 5),
               "result payload");

  writer.write(make_shutdown_frame());
  expect(loop.get() == WorkerLoopExit::shutdown, "SHUTDOWN ends the loop");
}

void test_cancel_and_protocol_error() {
  SocketPair sockets;
  ToyRuntime runtime;
  auto loop = std::async(std::launch::async, [&] {
    return run_worker_loop(sockets.worker, sockets.worker, runtime.registry, runtime.store,
                           WorkerLoopOptions{.worker_session_id = "ws_loop"});
  });
  FdFrameWriter writer(sockets.coordinator);
  FdFrameReader reader(sockets.coordinator);

  writer.write(make_leased_assign_frame(sleeping_spec(), lease("lease_2", 1)));
  writer.write(make_cancel_frame("lease_2"));
  // A coordinator never sends HEARTBEAT; the worker must end the session.
  writer.write(make_heartbeat_frame("lease_2"));
  expect(loop.get() == WorkerLoopExit::protocol_error, "unexpected frame is a protocol error");
  ::close(sockets.worker);
  sockets.worker = -1;

  bool saw_error = false;
  while (std::optional<Frame> frame = reader.read()) {
    expect(frame->type != MessageType::result, "a cancelled lease sends no result");
    saw_error = saw_error || frame->type == MessageType::error;
  }
  expect(saw_error, "ERROR frame sent before the session ended");
}

void test_end_of_input_closes_session() {
  SocketPair sockets;
  ToyRuntime runtime;
  auto loop = std::async(std::launch::async, [&] {
    return run_worker_loop(sockets.worker, sockets.worker, runtime.registry, runtime.store,
                           WorkerLoopOptions{.worker_session_id = "ws_loop"});
  });
  ::shutdown(sockets.coordinator, SHUT_WR);
  expect(loop.get() == WorkerLoopExit::input_closed, "end of input ends the loop");
}

}  // namespace

int main() {
  return run_tests("svp-exec-worker-loop-tests",
                   {
                       {"assign, heartbeat, result", test_assign_heartbeat_result},
                       {"cancel and protocol error", test_cancel_and_protocol_error},
                       {"end of input closes session", test_end_of_input_closes_session},
                   });
}
