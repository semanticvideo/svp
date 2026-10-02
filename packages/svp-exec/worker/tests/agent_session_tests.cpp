// The worker agent's session (plan §4.3, §4.4) end to end over a socket
// pair: HELLO refusal on an OS mismatch, runtime push and verification, a
// toy task run by a session process spawned from the pushed runtime, memory
// admission, model bundle push with lock verification, and cleanup.
//
//   svp-exec-worker-agent-session-tests <svp-exec-test-worker>

#include "svp/exec/lease_frames.hpp"
#include "svp/exec/task_frames.hpp"
#include "svp/exec/worker/agent_session.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/runtime_source.hpp"
#include "svp/exec/worker/transfer_messages.hpp"
#include "toy_tasks.hpp"
#include "worker_test_support.hpp"

#include <atomic>
#include <csignal>
#include <fstream>
#include <thread>

namespace {

using namespace svp::exec;
using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
namespace fs = std::filesystem;

constexpr std::uint64_t kGiB = 1ULL << 30;

fs::path g_test_worker;

class AgentHarness {
 public:
  explicit AgentHarness(SlotSharing::LocalLoad local_load = {})
      : scratch_("svp-agent-session") {
    core_ = std::make_unique<AgentCore>(AgentCoreOptions{
        .layout = WorkerLayout{.root = scratch_.path / "Worker"},
        .host = detect_host_facts(),
        .admission = {},
        .agent_runtime_id = std::nullopt,
        .shutdown_grace = std::chrono::milliseconds{2'000},
        .frame_limits = {},
        .sample_memory =
            [this] {
              return MemorySnapshot{.available_bytes = available_.load(),
                                    .pressure = MemoryPressure::normal};
            },
        .launcher = {},
        .local_load = std::move(local_load)});
    agent_ = std::thread([this] {
      end_ = serve_agent_session(*core_, *channel_.right_reader, *channel_.right_writer,
                                 "ws-test-" + std::to_string(++sessions_),
                                 [this] { ::shutdown(channel_.fds[1], SHUT_RDWR); });
    });
  }

  ~AgentHarness() { finish(); }

  WorkerSessionClient client() {
    return WorkerSessionClient(*channel_.left_reader, *channel_.left_writer);
  }
  FrameReader& reader() { return *channel_.left_reader; }
  FrameWriter& writer() { return *channel_.left_writer; }
  AgentCore& core() { return *core_; }
  const fs::path& scratch() const { return scratch_.path; }
  void set_available(std::uint64_t bytes) { available_ = bytes; }

  AgentSessionEnd finish() {
    if (agent_.joinable()) {
      channel_.close_left();
      agent_.join();
    }
    return end_;
  }
  // Waits for the agent to end the session on its own.
  AgentSessionEnd wait() {
    if (agent_.joinable()) {
      agent_.join();
    }
    return end_;
  }

 private:
  TemporaryDirectory scratch_;
  std::unique_ptr<AgentCore> core_;
  FrameChannel channel_;
  std::atomic<std::uint64_t> available_{64 * kGiB};
  std::thread agent_;
  AgentSessionEnd end_ = AgentSessionEnd::protocol_error;
  inline static std::atomic<int> sessions_{0};
};

CoordinatorRuntime toy_runtime() {
  return single_program_runtime(g_test_worker, "svp-exec-test-worker",
                                "test stand-in for svp-builder");
}

CoordinatorHello hello_for(const CoordinatorRuntime& runtime) {
  return make_coordinator_hello(runtime, fixed_thread_plan(), std::nullopt);
}

TaskSpec toy_spec(const std::string& task_id, std::uint64_t seed) {
  return svp::exec::test::make_toy_node(svp::exec::test::ToyTask{
                                            .task_id = task_id,
                                            .seed = seed,
                                            .order_key = {.lane = "alpha", .ordinals = {seed}}})
      .spec;
}

Lease lease(const std::string& id) {
  return Lease{.lease_id = id,
               .attempt = 1,
               .duration = std::chrono::milliseconds{30'000},
               .heartbeat_interval = std::chrono::milliseconds{500}};
}

// The next frame that is not a HEARTBEAT.
Frame next_reply(FrameReader& reader) {
  while (true) {
    std::optional<Frame> frame = reader.read();
    expect(frame.has_value(), "the agent answered");
    if (frame->type != MessageType::heartbeat) {
      return std::move(*frame);
    }
  }
}

std::size_t entries(const fs::path& directory) {
  std::error_code error;
  std::size_t count = 0;
  for (auto it = fs::directory_iterator(directory, error); !error && it != fs::directory_iterator();
       it.increment(error)) {
    ++count;
  }
  return count;
}

void test_os_mismatch_refuses_the_session() {
  AgentHarness agent;
  CoordinatorHello hello = hello_for(toy_runtime());
  hello.host.os.product_version = "0.0";
  WorkerSessionClient client = agent.client();
  try {
    (void)client.hello(hello);
    throw std::runtime_error("HELLO from another macOS version was accepted");
  } catch (const WorkerError& error) {
    expect(error.code() == WorkerErrorCode::refused, "refused");
    expect(std::string(error.what()).find("os_mismatch") != std::string::npos,
           std::string("names os_mismatch: ") + error.what());
  }
  expect(agent.wait() == AgentSessionEnd::refused, "the agent ends a refused session");
}

void test_pushed_runtime_runs_a_toy_task() {
  AgentHarness agent;
  const CoordinatorRuntime runtime = toy_runtime();
  WorkerSessionClient client = agent.client();
  const WorkerHelloAck ack = client.hello(hello_for(runtime));
  expect(!ack.runtime_present, "the worker starts without the runtime");
  expect(ack.host.logical_cpus > 0 && ack.memory_reserve_bytes > 0,
         "HELLO_ACK carries what calibration needs");
  TransferStats stats;
  client.ensure_runtime(runtime, stats);
  expect(stats.runtime_pushed && stats.blobs_sent == 2, "program and manifest sent");
  expect(agent.core().runtimes().has(runtime.runtime_id), "runtime installed");
  agent.core().runtimes().verify(runtime.runtime_id);
  TransferStats again;
  client.ensure_runtime(runtime, again);
  expect(!again.runtime_pushed && again.bytes_sent == 0, "an installed runtime is not resent");

  const TaskSpec spec = toy_spec("task.toy.a_000", 7);
  agent.writer().write(make_leased_assign_frame(spec, lease("lease-1")));
  const Frame reply = next_reply(agent.reader());
  expect(reply.type == MessageType::result, "the task produced a RESULT");
  const TaskResult result = task_result_from_result_frame(reply);
  expect(result.status == TaskStatus::succeeded, "toy task succeeded");
  expect(result.execution.runtime_id == runtime.runtime_id, "result names the pushed runtime");
  expect(svp::exec::test::to_text(reply.payloads.front()) ==
             svp::exec::test::toy_expected_output("task.toy.a_000", 7),
         "output is the toy task's expected bytes");
  expect(entries(agent.core().layout().sessions()) == 1, "session scratch exists while running");
  client.shutdown();
  expect(agent.wait() == AgentSessionEnd::shutdown, "SHUTDOWN ends the session");
  expect(entries(agent.core().layout().sessions()) == 0, "session scratch removed");
}

void test_admission_rejects_a_task_that_does_not_fit() {
  AgentHarness agent;
  const CoordinatorRuntime runtime = toy_runtime();
  WorkerSessionClient client = agent.client();
  (void)client.hello(hello_for(runtime));
  TransferStats stats;
  client.ensure_runtime(runtime, stats);
  agent.set_available(3 * kGiB);
  TaskSpec big = toy_spec("task.toy.a_001", 8);
  big.resources.est_peak_rss_mb = 4096;
  agent.writer().write(make_leased_assign_frame(big, lease("lease-big")));
  const Frame reply = next_reply(agent.reader());
  expect(reply.type == MessageType::reject, "REJECT");
  const LeaseRejection rejection = lease_rejection_from_frame(reply);
  expect(rejection.lease_id == "lease-big" && rejection.code == kRejectInsufficientMemory,
         "insufficient_memory for that lease");
  expect(agent.core().ledger().committed_bytes() == 0, "nothing admitted");

  const TaskSpec small = toy_spec("task.toy.a_002", 9);
  agent.writer().write(make_leased_assign_frame(small, lease("lease-small")));
  expect(next_reply(agent.reader()).type == MessageType::result, "a task that fits runs");
  expect(agent.core().ledger().committed_bytes() == 0, "finished lease released");
}

void test_declared_slots_are_shared_with_the_local_build() {
  // This Mac's own build runs one toy task: a coordinator that declared one
  // toy slot finds it taken, one that declared two gets the second.
  AgentHarness agent([] {
    return TaskTypeCounts{{std::string(svp::exec::test::kToyTaskType), 1}};
  });
  const CoordinatorRuntime runtime = toy_runtime();
  WorkerSessionClient client = agent.client();
  CoordinatorHello hello = hello_for(runtime);
  hello.capacity = {{std::string(svp::exec::test::kToyTaskType), 1}};
  (void)client.hello(hello);
  TransferStats stats;
  client.ensure_runtime(runtime, stats);
  agent.writer().write(make_leased_assign_frame(toy_spec("task.toy.a_001", 8), lease("l1")));
  const Frame reply = next_reply(agent.reader());
  expect(reply.type == MessageType::reject &&
             lease_rejection_from_frame(reply).code == kRejectInsufficientSlots,
         "the only declared slot is in use here");
  expect(agent.core().slots().held(svp::exec::test::kToyTaskType) == 0, "nothing held");
}

void test_blob_get_returns_a_verified_blob() {
  AgentHarness agent;
  WorkerSessionClient client = agent.client();
  (void)client.hello(hello_for(toy_runtime()));
  const fs::path source = agent.scratch() / "package.bin";
  std::string content;
  for (int index = 0; index < 1000; ++index) {
    content += "package bytes " + std::to_string(index) + "\n";
  }
  write_file(source, content);
  const BlobRef blob = blob_ref_for(svp::exec::test::to_bytes(content));
  TransferStats stats;
  client.send_blobs({BlobSource{.ref = blob, .file = source}}, stats);
  const fs::path fetched = agent.scratch() / "fetched.bin";
  client.fetch_blob(blob, fetched);
  expect(read_file(fetched) == content, "the fetched blob is the stored blob");
  BlobRef absent = blob_ref_for(svp::exec::test::to_bytes("never stored"));
  expect_worker_error(
      WorkerErrorCode::configuration, [&] { client.fetch_blob(absent, fetched); },
      "a blob the worker does not hold");
  expect(read_file(fetched) == content, "a failed fetch leaves the destination alone");
}

void test_tampered_runtime_is_never_run() {
  AgentHarness agent;
  const CoordinatorRuntime runtime = toy_runtime();
  WorkerSessionClient client = agent.client();
  (void)client.hello(hello_for(runtime));
  TransferStats stats;
  client.ensure_runtime(runtime, stats);
  const fs::path program = agent.core().runtimes().directory_of(runtime.runtime_id) / "bin/svp-builder";
  fs::permissions(program, fs::perms::owner_write, fs::perm_options::add);
  {
    std::ofstream append(program, std::ios::binary | std::ios::app);
    append << "tampered";
  }
  agent.writer().write(make_leased_assign_frame(toy_spec("task.toy.a_003", 10), lease("l")));
  const Frame reply = next_reply(agent.reader());
  expect(reply.type == MessageType::error, "ERROR instead of running");
  expect(reply.body.at("code") == "verification", "verification error: " + reply.body.dump());
  expect(agent.wait() == AgentSessionEnd::protocol_error, "session ends");
}

void test_bad_blob_is_refused() {
  AgentHarness agent;
  WorkerSessionClient client = agent.client();
  (void)client.hello(hello_for(toy_runtime()));
  const std::vector<std::byte> bytes = svp::exec::test::to_bytes("real content");
  BlobRef claimed = blob_ref_for(svp::exec::test::to_bytes("other content"));
  claimed.bytes = bytes.size();
  agent.writer().write(make_blob_chunk_frame(BlobChunk{.blob = claimed, .offset = 0}, bytes));
  agent.writer().write(make_blob_query_frame(BlobQuery{.blobs = {claimed}}));
  const Frame reply = next_reply(agent.reader());
  expect(reply.type == MessageType::error && reply.body.at("code") == "verification",
         "a blob that does not hash to its name is refused: " + reply.body.dump());
  expect(!agent.core().cas().has(claimed.blake3), "nothing stored");
  expect(agent.wait() == AgentSessionEnd::protocol_error, "session ends");
  expect(entries(agent.core().layout().sessions()) == 0, "partial blob removed");
}

void test_model_bundle_push_is_lock_verified() {
  AgentHarness agent;
  const fs::path cache = agent.scratch() / "coordinator-models";
  const nlohmann::json a = write_model_bundle(cache, "model_worker_test_a", "weights a\n");
  const nlohmann::json b = write_model_bundle(cache, "model_worker_test_b", "weights b\n");
  write_model_lock(cache, {a, b});
  const std::vector<ModelBundleSource> bundles = prepare_model_bundles(cache, {"model_worker_test_a"});
  expect(bundles.size() == 1, "one bundle selected");

  WorkerSessionClient client = agent.client();
  CoordinatorHello hello = hello_for(toy_runtime());
  hello.model_set = model_set_summary(cache);
  expect(hello.model_set && hello.model_set->model_set_id == "worker-test-set", "model set summary");
  (void)client.hello(hello);
  TransferStats stats;
  client.ensure_model_bundles(bundles, stats);
  expect(stats.model_bundles_pushed.size() == 1, "bundle pushed");
  expect(agent.core().models().has(bundles.front().bundle_blake3), "bundle installed");
  agent.core().models().verify(bundles.front().bundle_blake3);
  TransferStats again;
  client.ensure_model_bundles(bundles, again);
  expect(again.model_bundles_pushed.empty() && again.bytes_sent == 0, "not resent");

  // A manifest that does not match the lock entry fails the worker's lock
  // verification.
  ModelBundleSource forged = prepare_model_bundles(cache, {"model_worker_test_b"}).front();
  forged.manifest = bundles.front().manifest;
  try {
    client.ensure_model_bundles({forged}, again);
    throw std::runtime_error("a forged bundle was installed");
  } catch (const WorkerError& error) {
    expect(std::string(error.what()).find("verification") != std::string::npos,
           std::string("lock verification failed it: ") + error.what());
  }
  expect(!agent.core().models().has(forged.bundle_blake3), "forged bundle not installed");
}

void test_disconnect_stops_the_session_process() {
  AgentHarness agent;
  const CoordinatorRuntime runtime = toy_runtime();
  WorkerSessionClient client = agent.client();
  (void)client.hello(hello_for(runtime));
  TransferStats stats;
  client.ensure_runtime(runtime, stats);
  agent.writer().write(make_leased_assign_frame(toy_spec("task.toy.a_004", 11), lease("l4")));
  expect(next_reply(agent.reader()).type == MessageType::result, "session process running");
  expect(agent.finish() == AgentSessionEnd::input_closed, "disconnect ends the session");
  expect(entries(agent.core().layout().sessions()) == 0, "scratch removed on disconnect");
  expect(agent.core().active_sessions.load() == 0, "no active sessions");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: svp-exec-worker-agent-session-tests <svp-exec-test-worker>\n";
    return 2;
  }
  g_test_worker = fs::absolute(argv[1]);
  std::signal(SIGPIPE, SIG_IGN);
  return run_tests(
      "svp-exec-worker-agent-session-tests",
      {
          {"OS mismatch refuses the session", test_os_mismatch_refuses_the_session},
          {"pushed runtime runs a toy task", test_pushed_runtime_runs_a_toy_task},
          {"admission rejects a task that does not fit",
           test_admission_rejects_a_task_that_does_not_fit},
          {"declared slots are shared with the local build",
           test_declared_slots_are_shared_with_the_local_build},
          {"BLOB_GET returns a verified blob", test_blob_get_returns_a_verified_blob},
          {"tampered runtime is never run", test_tampered_runtime_is_never_run},
          {"bad blob is refused", test_bad_blob_is_refused},
          {"model bundle push is lock-verified", test_model_bundle_push_is_lock_verified},
          {"disconnect stops the session process", test_disconnect_stops_the_session_process},
      });
}
