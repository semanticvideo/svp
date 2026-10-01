#pragma once

// HELLO / HELLO_ACK (plan §4.1, §4.3): the first exchange of every worker
// session, before any runtime, blob, or lease moves.
//
// HELLO (coordinator -> worker), no payloads:
//   {"host":{HostFacts},
//    "model_set":{"model_lock_blake3":"<hex>","model_set_id":"..."},  optional
//    "protocol":{"major":M,"minor":m},
//    "runtime_id":"b3:<hex>",
//    "runtime_kind":"bundle"|"builder_only",
//    "thread_plan":{"host_independent":bool,"plan":{ThreadPlan JSON}}}
//
// HELLO_ACK (worker -> coordinator), no payloads:
//   {"accepted":bool,
//    "agent_runtime_id":"b3:<hex>",                 optional
//    "cache":{"blob_count":n,"total_bytes":n},
//    "disk":{"available_bytes":n},
//    "host":{HostFacts},
//    "memory":{"available_bytes":n,"pressure":"normal"|"warning"|"critical",
//              "reserve_bytes":n},
//    "model_bundles":["<bundle_blake3 hex>", ...],
//    "protocol":{"major":M,"minor":m},
//    "refusal":{"code":"...","message":"..."},      only when accepted=false
//    "runtime_present":bool,
//    "runtimes":["b3:<hex>", ...],
//    "sessions":{"active":n}}
//
// HostFacts JSON:
//   {"arch","cpu_brand","efficiency_cpus","logical_cpus",
//    "os":{"build","product_version"},"performance_cpus",
//    "physical_memory_bytes"}
//
// Unlike task and lease frames, unknown members are ignored here: these two
// messages are where peers of different minor protocol versions meet, and a
// newer minor may add members an older peer does not know (plan §4.1:
// "majors must match, minor may differ"). Everything carries what capacity
// calibration (a later step) needs: logical CPUs and their performance /
// efficiency split, physical and available memory, and the coordinator's
// thread plan.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/frame.hpp"
#include "svp/exec/worker/host_facts.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

struct ProtocolVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;

  bool operator==(const ProtocolVersion&) const = default;
};

// 1.0: HELLO / HELLO_ACK, RUNTIME_HAVE / RUNTIME_PUT, BLOB_HAVE / BLOB_PUT,
// REJECT, and the lease messages of svp-exec (lease_frames.hpp).
inline constexpr ProtocolVersion kWorkerProtocolVersion{1, 0};

[[nodiscard]] bool protocol_compatible(ProtocolVersion local, ProtocolVersion peer) noexcept;

// How the coordinator's runtime was assembled (plan §3.2): a full bundle
// (svp-builder plus the pinned ffmpeg, ffprobe, and sherpa-onnx), or only
// svp-builder because no bundle is installed next to it. With builder_only,
// a worker's svp-builder resolves those tools the same way a local build
// without a bundle does (runtime_tools.hpp); the kind is recorded so reports
// show the fallback.
enum class RuntimeKind { bundle, builder_only };

[[nodiscard]] std::string_view runtime_kind_name(RuntimeKind kind) noexcept;
[[nodiscard]] std::optional<RuntimeKind> parse_runtime_kind(std::string_view name) noexcept;

struct ModelSetSummary {
  std::string model_set_id;
  // BLAKE3 of the coordinator's model-lock.json bytes.
  Blake3Digest model_lock_blake3{};

  bool operator==(const ModelSetSummary&) const = default;
};

struct CoordinatorHello {
  ProtocolVersion protocol = kWorkerProtocolVersion;
  Blake3Digest runtime_id{};
  RuntimeKind runtime_kind = RuntimeKind::bundle;
  HostFacts host;
  // svp::models::thread_plan_to_json of the coordinator's plan, and whether
  // it fixes every runtime's thread count (thread_plan_is_host_independent).
  nlohmann::json thread_plan = nlohmann::json::object();
  bool thread_plan_host_independent = false;
  std::optional<ModelSetSummary> model_set;

  bool operator==(const CoordinatorHello&) const = default;
};

// Why a worker refuses a whole session (plan §4.1 mismatch behaviour).
enum class SessionRefusalCode {
  // Protocol majors differ.
  protocol_mismatch,
  // macOS product versions differ (plan §4.1 `os`: "worker refuses work and
  // reports it").
  os_mismatch,
  // CPU architectures differ (plan §0: Apple Silicon only).
  arch_mismatch,
  // The coordinator's thread plan leaves a count to the host, so output
  // could depend on which Mac ran a task (plan §3.5).
  thread_plan_host_dependent,
};

[[nodiscard]] std::string_view session_refusal_code_name(SessionRefusalCode code) noexcept;
[[nodiscard]] std::optional<SessionRefusalCode> parse_session_refusal_code(
    std::string_view name) noexcept;

struct SessionRefusal {
  SessionRefusalCode code = SessionRefusalCode::protocol_mismatch;
  std::string message;

  bool operator==(const SessionRefusal&) const = default;
};

struct WorkerHelloAck {
  ProtocolVersion protocol = kWorkerProtocolVersion;
  // Set exactly when the worker refused the session.
  std::optional<SessionRefusal> refusal;
  HostFacts host;
  MemorySnapshot memory;
  // Admission reserve this worker keeps free (admission.hpp).
  std::uint64_t memory_reserve_bytes = 0;
  std::uint64_t disk_available_bytes = 0;
  std::uint64_t cache_blob_count = 0;
  std::uint64_t cache_total_bytes = 0;
  // Whether HELLO's runtime_id is installed and its manifest decodes.
  bool runtime_present = false;
  std::vector<Blake3Digest> runtimes;
  // bundle_blake3 of every verified model bundle the worker holds.
  std::vector<Blake3Digest> model_bundles;
  std::uint64_t active_sessions = 0;
  // The runtime the worker agent itself runs from, when it knows it.
  std::optional<Blake3Digest> agent_runtime_id;

  [[nodiscard]] bool accepted() const noexcept { return !refusal.has_value(); }
  bool operator==(const WorkerHelloAck&) const = default;
};

[[nodiscard]] nlohmann::json host_facts_to_json(const HostFacts& facts);
[[nodiscard]] HostFacts host_facts_from_json(const nlohmann::json& value, std::string_view path);

[[nodiscard]] Frame make_hello_frame(const CoordinatorHello& hello);
// Throws ExecError (frame_malformed, missing_field, wrong_type,
// invalid_value, invalid_digest) for a body that is not a HELLO.
[[nodiscard]] CoordinatorHello hello_from_frame(const Frame& frame);

[[nodiscard]] Frame make_hello_ack_frame(const WorkerHelloAck& ack);
[[nodiscard]] WorkerHelloAck hello_ack_from_frame(const Frame& frame);

// The worker's session decision: nullopt to accept, otherwise why not.
// Checks, in order: protocol major, CPU architecture, macOS product version,
// and that the thread plan parses, is host-independent, and agrees with the
// flag HELLO carries.
[[nodiscard]] std::optional<SessionRefusal> evaluate_hello(const CoordinatorHello& hello,
                                                           const HostFacts& worker);

}  // namespace svp::exec::worker
