// The worker handshake and transfer codecs (plan §4.1, §4.3): every message
// survives the wire, HELLO's compatibility rules refuse what they must, and
// REJECT reaches the scheduler as a lost attempt.

#include "svp/exec/frame_decoder.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/transfer_messages.hpp"
#include "svp/exec/worker_session_leases.hpp"
#include "worker_test_support.hpp"

#include <mutex>
#include <tuple>

namespace {

using namespace svp::exec;
using namespace svp::exec::worker;
using namespace svp::exec::worker::test;

// Encodes and decodes a frame, as the wire does.
Frame wire(const Frame& frame) {
  const std::vector<std::byte> bytes = encode_frame(frame);
  FrameDecoder decoder;
  decoder.feed(bytes);
  std::optional<Frame> decoded = decoder.next();
  expect(decoded.has_value(), "one whole frame decodes");
  return std::move(*decoded);
}

HostFacts sample_host() {
  return HostFacts{.arch = "arm64",
                   .os = OsIdentity{.product_version = "27.0.1", .build = "26A434"},
                   .logical_cpus = 10,
                   .performance_cpus = 4,
                   .efficiency_cpus = 6,
                   .physical_memory_bytes = 16ULL << 30,
                   .cpu_brand = "Apple M4"};
}

CoordinatorHello sample_hello() {
  CoordinatorHello hello;
  hello.runtime_id = blake3_digest(std::string_view("runtime"));
  hello.runtime_kind = RuntimeKind::builder_only;
  hello.host = sample_host();
  hello.thread_plan = svp::models::thread_plan_to_json(fixed_thread_plan());
  hello.thread_plan_host_independent = true;
  hello.model_set = ModelSetSummary{.model_set_id = "set",
                                    .model_lock_blake3 = blake3_digest(std::string_view("lock"))};
  return hello;
}

void test_hello_round_trips() {
  const CoordinatorHello hello = sample_hello();
  expect(hello_from_frame(wire(make_hello_frame(hello))) == hello, "HELLO round trips");
  CoordinatorHello bare = hello;
  bare.model_set.reset();
  expect(hello_from_frame(wire(make_hello_frame(bare))) == bare, "HELLO without model set");
}

void test_hello_ignores_members_a_newer_minor_adds() {
  Frame frame = make_hello_frame(sample_hello());
  frame.body["protocol"]["minor"] = kWorkerProtocolVersion.minor + 1;
  frame.body["calibration_hint"] = nlohmann::json{{"future", true}};
  const CoordinatorHello decoded = hello_from_frame(wire(frame));
  expect(decoded.protocol.minor == kWorkerProtocolVersion.minor + 1, "newer minor read");
  expect(!evaluate_hello(decoded, sample_host()), "a newer minor is still accepted");
}

void test_hello_ack_round_trips() {
  WorkerHelloAck ack;
  ack.host = sample_host();
  ack.memory = MemorySnapshot{.available_bytes = 7ULL << 30, .pressure = MemoryPressure::warning};
  ack.memory_reserve_bytes = 2ULL << 30;
  ack.disk_available_bytes = 100ULL << 30;
  ack.cache_blob_count = 3;
  ack.cache_total_bytes = 4096;
  ack.runtime_present = true;
  ack.runtimes = {blake3_digest(std::string_view("a")), blake3_digest(std::string_view("b"))};
  ack.model_bundles = {blake3_digest(std::string_view("m"))};
  ack.active_sessions = 2;
  ack.agent_runtime_id = blake3_digest(std::string_view("agent"));
  expect(hello_ack_from_frame(wire(make_hello_ack_frame(ack))) == ack, "accepting ack");
  ack.refusal = SessionRefusal{.code = SessionRefusalCode::os_mismatch, .message = "differs"};
  ack.agent_runtime_id.reset();
  const WorkerHelloAck refused = hello_ack_from_frame(wire(make_hello_ack_frame(ack)));
  expect(refused == ack && !refused.accepted(), "refusing ack");

  Frame inconsistent = make_hello_ack_frame(ack);
  inconsistent.body["accepted"] = true;
  bool rejected = false;
  try {
    (void)hello_ack_from_frame(wire(inconsistent));
  } catch (const ExecError&) {
    rejected = true;
  }
  expect(rejected, "accepted=true with a refusal is malformed");
}

void test_os_mismatch_is_refused_and_named() {
  CoordinatorHello hello = sample_hello();
  hello.host.os = OsIdentity{.product_version = "27.1", .build = "26B100"};
  const std::optional<SessionRefusal> refusal = evaluate_hello(hello, sample_host());
  expect(refusal && refusal->code == SessionRefusalCode::os_mismatch, "os_mismatch");
  expect(refusal->message.find("27.0.1") != std::string::npos &&
             refusal->message.find("27.1") != std::string::npos,
         "both versions named: " + refusal->message);

  CoordinatorHello other_build = sample_hello();
  other_build.host.os.build = "26A999";
  expect(!evaluate_hello(other_build, sample_host()),
         "a different build of the same product version is accepted");
}

void test_other_refusals() {
  CoordinatorHello hello = sample_hello();
  hello.protocol.major = kWorkerProtocolVersion.major + 1;
  expect(evaluate_hello(hello, sample_host())->code == SessionRefusalCode::protocol_mismatch,
         "protocol major mismatch");

  hello = sample_hello();
  hello.host.arch = "x86_64";
  expect(evaluate_hello(hello, sample_host())->code == SessionRefusalCode::arch_mismatch,
         "arch mismatch");

  hello = sample_hello();
  svp::models::ThreadPlan plan = fixed_thread_plan();
  plan.ocr_detection.intra_op = svp::models::kRuntimeChoosesThreadCount;
  hello.thread_plan = svp::models::thread_plan_to_json(plan);
  expect(evaluate_hello(hello, sample_host())->code ==
             SessionRefusalCode::thread_plan_host_dependent,
         "a plan leaving a count to the host is refused");

  hello = sample_hello();
  hello.thread_plan_host_independent = false;
  expect(evaluate_hello(hello, sample_host())->code ==
             SessionRefusalCode::thread_plan_host_dependent,
         "a coordinator that says its plan is host-dependent is refused");

  hello = sample_hello();
  hello.thread_plan = nlohmann::json{{"nonsense", 1}};
  expect(evaluate_hello(hello, sample_host())->code ==
             SessionRefusalCode::thread_plan_host_dependent,
         "an unparseable plan is refused");
}

void test_transfer_messages_round_trip() {
  const Blake3Digest runtime = blake3_digest(std::string_view("runtime"));
  expect(runtime_query_from_frame(wire(make_runtime_query_frame(runtime))) == runtime,
         "RUNTIME_HAVE query");
  const RuntimeAnswer answer{.runtime_id = runtime, .present = true};
  expect(runtime_answer_from_frame(wire(make_runtime_answer_frame(answer))) == answer,
         "RUNTIME_HAVE answer");
  const BlobRef manifest = blob_ref_for(svp::exec::test::to_bytes("manifest"));
  RuntimePut put{.runtime_id = runtime, .manifest = manifest, .components = std::nullopt};
  expect(runtime_put_from_frame(wire(make_runtime_put_frame(put))) == put, "RUNTIME_PUT");
  put.components = blob_ref_for(svp::exec::test::to_bytes("components"));
  expect(runtime_put_from_frame(wire(make_runtime_put_frame(put))) == put,
         "RUNTIME_PUT with components");

  const BlobQuery blobs{.blobs = {manifest, *put.components}};
  expect(std::get<BlobQuery>(blob_have_query_from_frame(wire(make_blob_query_frame(blobs)))) ==
             blobs,
         "BLOB_HAVE blob query");
  expect(blob_answer_from_frame(wire(make_blob_answer_frame(blobs))) == blobs,
         "BLOB_HAVE blob answer");
  const ModelBundleQuery bundles{.bundles = {runtime}};
  expect(std::get<ModelBundleQuery>(
             blob_have_query_from_frame(wire(make_model_bundle_query_frame(bundles)))) == bundles,
         "BLOB_HAVE model query");
  expect(model_bundle_answer_from_frame(wire(make_model_bundle_answer_frame(bundles))) == bundles,
         "BLOB_HAVE model answer");

  const std::vector<std::byte> data = svp::exec::test::to_bytes("0123456789");
  const BlobRef blob = blob_ref_for(data);
  const Frame chunk_frame = wire(make_blob_chunk_frame(
      BlobChunk{.blob = blob, .offset = 4},
      std::vector<std::byte>(data.begin() + 4, data.end())));
  const BlobPut chunk = blob_put_from_frame(chunk_frame);
  expect(std::get<BlobChunk>(chunk) == (BlobChunk{.blob = blob, .offset = 4}), "BLOB_PUT chunk");
  expect(chunk_frame.payloads.front().size() == 6, "chunk payload");

  const ModelBundlePut model{.lock = nlohmann::json{{"models", nlohmann::json::array()}},
                             .manifest = manifest};
  expect(std::get<ModelBundlePut>(blob_put_from_frame(wire(make_model_bundle_put_frame(model)))) ==
             model,
         "BLOB_PUT model bundle");
}

void test_transfer_messages_are_strict() {
  const std::vector<std::byte> data = svp::exec::test::to_bytes("0123456789");
  const BlobRef blob = blob_ref_for(data);
  svp::exec::test::expect_exec_error(
      ExecErrorCode::invalid_value,
      [&] {
        (void)make_blob_chunk_frame(BlobChunk{.blob = blob, .offset = 8},
                                    std::vector<std::byte>(4, std::byte{0}));
      },
      "a chunk past the blob's end is refused");
  Frame query = make_runtime_query_frame(blob.blake3);
  query.body["path"] = "/etc/passwd";
  svp::exec::test::expect_exec_error(ExecErrorCode::unknown_field,
                                     [&] { (void)runtime_query_from_frame(wire(query)); },
                                     "transfer bodies reject unknown members such as paths");
  Frame put = make_blob_query_frame(BlobQuery{});
  put.body["kind"] = "command";
  svp::exec::test::expect_exec_error(ExecErrorCode::invalid_value,
                                     [&] { (void)blob_have_query_from_frame(wire(put)); },
                                     "unknown kinds are refused");
}

class RecordingEvents final : public ExecutorEvents {
 public:
  void lease_heartbeat(std::string_view) override {}
  void attempt_finished(std::string_view, AttemptOutput) override {}
  void attempt_failed(std::string_view lease_id, AttemptFailureKind kind,
                      std::string message) override {
    failures.emplace_back(std::string(lease_id), kind, std::move(message));
  }
  std::vector<std::tuple<std::string, AttemptFailureKind, std::string>> failures;
};

class VectorReader final : public FrameReader {
 public:
  explicit VectorReader(std::vector<Frame> frames) : frames_(std::move(frames)) {}
  std::optional<Frame> read() override {
    if (next_ >= frames_.size()) {
      return std::nullopt;
    }
    return frames_[next_++];
  }

 private:
  std::vector<Frame> frames_;
  std::size_t next_ = 0;
};

void test_reject_fails_only_that_attempt() {
  const LeaseRejection rejection{.lease_id = "lease-1",
                                 .code = std::string(kRejectInsufficientMemory),
                                 .message = "too big"};
  expect(lease_rejection_from_frame(wire(make_reject_frame(rejection))) == rejection,
         "REJECT round trips");
  svp::exec::test::expect_exec_error(
      ExecErrorCode::invalid_value,
      [] { (void)make_reject_frame(LeaseRejection{.lease_id = "l", .code = "Bad Code"}); },
      "REJECT codes are lower-case identifiers");

  WorkerSessionLeases leases;
  leases.add("lease-1", "task-1", 1);
  leases.add("lease-2", "task-2", 1);
  std::mutex mutex;
  RecordingEvents events;
  VectorReader reader({make_reject_frame(rejection)});
  const WorkerSessionEnd end = pump_worker_frames(reader, mutex, leases, events, "ended");
  expect(events.failures.size() == 1, "one attempt failed");
  expect(std::get<0>(events.failures.front()) == "lease-1" &&
             std::get<1>(events.failures.front()) == AttemptFailureKind::executor_lost,
         "the rejected lease is lost, not invalid");
  expect(std::get<2>(events.failures.front()).find("insufficient_memory") != std::string::npos,
         "reason names the rejection code");
  expect(end.failure == AttemptFailureKind::executor_lost, "session ends normally afterwards");
  expect(leases.contains("lease-2") && !leases.contains("lease-1"), "other lease untouched");

  VectorReader stray({make_reject_frame(LeaseRejection{.lease_id = "nobody", .code = "x"})});
  const WorkerSessionEnd stray_end = pump_worker_frames(stray, mutex, leases, events, "ended");
  expect(stray_end.failure == AttemptFailureKind::invalid_result,
         "a REJECT for an unknown lease is invalid");
}

}  // namespace

int main() {
  return run_tests("svp-exec-worker-protocol-tests",
                   {
                       {"HELLO round trips", test_hello_round_trips},
                       {"HELLO ignores members a newer minor adds",
                        test_hello_ignores_members_a_newer_minor_adds},
                       {"HELLO_ACK round trips", test_hello_ack_round_trips},
                       {"OS mismatch is refused and named", test_os_mismatch_is_refused_and_named},
                       {"other refusals", test_other_refusals},
                       {"transfer messages round trip", test_transfer_messages_round_trip},
                       {"transfer messages are strict", test_transfer_messages_are_strict},
                       {"REJECT fails only that attempt", test_reject_fails_only_that_attempt},
                   });
}
