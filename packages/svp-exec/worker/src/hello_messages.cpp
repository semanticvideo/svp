#include "svp/exec/worker/hello_messages.hpp"

#include "message_fields.hpp"
#include "svp/models/thread_plan.hpp"

#include <array>
#include <utility>

namespace svp::exec::worker {
namespace {

using namespace detail;

nlohmann::json protocol_to_json(ProtocolVersion version) {
  return nlohmann::json{{"major", version.major}, {"minor", version.minor}};
}

ProtocolVersion protocol_from_json(const nlohmann::json& body, std::string_view path) {
  const std::string protocol_path = child_path(path, "protocol");
  const nlohmann::json& value = required_object(body, "protocol", path);
  return ProtocolVersion{.major = required_u32(value, "major", protocol_path),
                         .minor = required_u32(value, "minor", protocol_path)};
}

nlohmann::json memory_to_json(const MemorySnapshot& memory, std::uint64_t reserve) {
  return nlohmann::json{{"available_bytes", memory.available_bytes},
                        {"pressure", std::string(memory_pressure_name(memory.pressure))},
                        {"reserve_bytes", reserve}};
}

constexpr std::array kRefusalCodes = {
    SessionRefusalCode::protocol_mismatch,
    SessionRefusalCode::os_mismatch,
    SessionRefusalCode::arch_mismatch,
    SessionRefusalCode::thread_plan_host_dependent,
};

}  // namespace

nlohmann::json pairing_proof_to_json(const PairingProof& proof) {
  return nlohmann::json{{"id", proof.pairing_id}, {"proof", proof.proof}};
}

bool protocol_compatible(ProtocolVersion local, ProtocolVersion peer) noexcept {
  return local.major == peer.major;
}

bool worker_accepts_blob_release(ProtocolVersion worker) noexcept {
  return worker.major == kWorkerProtocolVersion.major &&
         worker.minor >= kBlobReleaseMinorVersion;
}

std::string_view runtime_kind_name(RuntimeKind kind) noexcept {
  switch (kind) {
    case RuntimeKind::bundle:
      return "bundle";
    case RuntimeKind::builder_only:
      return "builder_only";
  }
  return "unknown";
}

std::optional<RuntimeKind> parse_runtime_kind(std::string_view name) noexcept {
  for (const RuntimeKind kind : {RuntimeKind::bundle, RuntimeKind::builder_only}) {
    if (runtime_kind_name(kind) == name) {
      return kind;
    }
  }
  return std::nullopt;
}

std::string_view session_refusal_code_name(SessionRefusalCode code) noexcept {
  switch (code) {
    case SessionRefusalCode::protocol_mismatch:
      return "protocol_mismatch";
    case SessionRefusalCode::os_mismatch:
      return "os_mismatch";
    case SessionRefusalCode::arch_mismatch:
      return "arch_mismatch";
    case SessionRefusalCode::thread_plan_host_dependent:
      return "thread_plan_host_dependent";
  }
  return "unknown";
}

std::optional<SessionRefusalCode> parse_session_refusal_code(std::string_view name) noexcept {
  for (const SessionRefusalCode code : kRefusalCodes) {
    if (session_refusal_code_name(code) == name) {
      return code;
    }
  }
  return std::nullopt;
}

nlohmann::json host_facts_to_json(const HostFacts& facts) {
  return nlohmann::json{
      {"arch", facts.arch},
      {"cpu_brand", facts.cpu_brand},
      {"efficiency_cpus", facts.efficiency_cpus},
      {"logical_cpus", facts.logical_cpus},
      {"os", nlohmann::json{{"build", facts.os.build},
                            {"product_version", facts.os.product_version}}},
      {"performance_cpus", facts.performance_cpus},
      {"physical_memory_bytes", facts.physical_memory_bytes},
  };
}

HostFacts host_facts_from_json(const nlohmann::json& value, std::string_view path) {
  require_object(value, path);
  HostFacts facts;
  facts.arch = required_string(value, "arch", path);
  facts.cpu_brand = required_string(value, "cpu_brand", path);
  facts.efficiency_cpus = required_u32(value, "efficiency_cpus", path);
  facts.logical_cpus = required_u32(value, "logical_cpus", path);
  facts.performance_cpus = required_u32(value, "performance_cpus", path);
  facts.physical_memory_bytes = required_unsigned(value, "physical_memory_bytes", path);
  const std::string os_path = child_path(path, "os");
  const nlohmann::json& os = required_object(value, "os", path);
  facts.os.build = required_string(os, "build", os_path);
  facts.os.product_version = required_string(os, "product_version", os_path);
  if (facts.arch.empty() || facts.os.product_version.empty() || facts.logical_cpus == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    std::string(path) + " needs arch, os.product_version, and logical_cpus");
  }
  return facts;
}

Frame make_hello_frame(const CoordinatorHello& hello) {
  nlohmann::json body{
      {"host", host_facts_to_json(hello.host)},
      {"protocol", protocol_to_json(hello.protocol)},
      {"runtime_id", blake3_prefixed(hello.runtime_id)},
      {"runtime_kind", std::string(runtime_kind_name(hello.runtime_kind))},
      {"thread_plan", nlohmann::json{{"host_independent", hello.thread_plan_host_independent},
                                     {"plan", hello.thread_plan}}},
  };
  if (hello.model_set) {
    body["model_set"] = nlohmann::json{
        {"model_lock_blake3", blake3_hex(hello.model_set->model_lock_blake3)},
        {"model_set_id", hello.model_set->model_set_id}};
  }
  if (!hello.capacity.empty()) {
    nlohmann::json capacity = nlohmann::json::object();
    for (const auto& [task_type, slots] : hello.capacity) {
      capacity[task_type] = slots;
    }
    body["capacity"] = std::move(capacity);
  }
  if (hello.pairing) {
    body["pairing"] = pairing_proof_to_json(*hello.pairing);
  }
  return Frame{.type = MessageType::hello, .body = std::move(body), .payloads = {}};
}

CoordinatorHello hello_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::hello, 0);
  const nlohmann::json& body = frame.body;
  CoordinatorHello hello;
  hello.protocol = protocol_from_json(body, path);
  hello.host = host_facts_from_json(required_field(body, "host", path), child_path(path, "host"));
  hello.runtime_id = required_blake3_prefixed(body, "runtime_id", path);
  const std::string kind = required_string(body, "runtime_kind", path);
  const std::optional<RuntimeKind> runtime_kind = parse_runtime_kind(kind);
  if (!runtime_kind) {
    throw ExecError(ExecErrorCode::invalid_value,
                    child_path(path, "runtime_kind") + " is not a runtime kind: `" + kind + "`");
  }
  hello.runtime_kind = *runtime_kind;
  const std::string plan_path = child_path(path, "thread_plan");
  const nlohmann::json& plan = required_object(body, "thread_plan", path);
  hello.thread_plan_host_independent = required_bool(plan, "host_independent", plan_path);
  hello.thread_plan = required_object(plan, "plan", plan_path);
  if (const auto model_set = body.find("model_set"); model_set != body.end()) {
    const std::string model_path = child_path(path, "model_set");
    require_object(*model_set, model_path);
    hello.model_set = ModelSetSummary{
        .model_set_id = required_string(*model_set, "model_set_id", model_path),
        .model_lock_blake3 = required_blake3_hex(*model_set, "model_lock_blake3", model_path)};
  }
  if (const auto capacity = body.find("capacity"); capacity != body.end()) {
    const std::string capacity_path = child_path(path, "capacity");
    require_object(*capacity, capacity_path);
    for (const auto& [task_type, slots] : capacity->items()) {
      const std::uint64_t value = required_unsigned(*capacity, task_type, capacity_path);
      if (task_type.empty() || value == 0) {
        throw ExecError(ExecErrorCode::invalid_value,
                        capacity_path + " needs a task type and at least one slot per entry");
      }
      hello.capacity.emplace(task_type, value);
    }
  }
  if (const auto pairing = body.find("pairing"); pairing != body.end()) {
    const std::string pairing_path = child_path(path, "pairing");
    require_object(*pairing, pairing_path);
    hello.pairing = PairingProof{.pairing_id = required_string(*pairing, "id", pairing_path),
                                 .proof = required_string(*pairing, "proof", pairing_path)};
  }
  return hello;
}

Frame make_hello_ack_frame(const WorkerHelloAck& ack) {
  nlohmann::json body{
      {"accepted", ack.accepted()},
      {"cache", nlohmann::json{{"blob_count", ack.cache_blob_count},
                               {"total_bytes", ack.cache_total_bytes}}},
      {"disk", nlohmann::json{{"available_bytes", ack.disk_available_bytes}}},
      {"host", host_facts_to_json(ack.host)},
      {"memory", memory_to_json(ack.memory, ack.memory_reserve_bytes)},
      {"model_bundles", digests_to_json(ack.model_bundles, false)},
      {"protocol", protocol_to_json(ack.protocol)},
      {"runtime_present", ack.runtime_present},
      {"runtimes", digests_to_json(ack.runtimes, true)},
      {"sessions", nlohmann::json{{"active", ack.active_sessions}}},
  };
  if (ack.refusal) {
    body["refusal"] =
        nlohmann::json{{"code", std::string(session_refusal_code_name(ack.refusal->code))},
                       {"message", ack.refusal->message}};
  }
  if (ack.agent_runtime_id) {
    body["agent_runtime_id"] = blake3_prefixed(*ack.agent_runtime_id);
  }
  if (!ack.worker_id.empty()) {
    body["worker_id"] = ack.worker_id;
  }
  if (ack.service) {
    nlohmann::json service{{"declined_runtimes", digests_to_json(ack.service->declined_runtimes, true)},
                           {"self_update", ack.service->self_update}};
    if (ack.service->release_stamp) {
      service["release_stamp"] = *ack.service->release_stamp;
    }
    if (ack.service->pending) {
      service["pending"] =
          nlohmann::json{{"bytes", ack.service->pending->bytes},
                         {"release_stamp", ack.service->pending->release_stamp},
                         {"runtime_id", blake3_prefixed(ack.service->pending->runtime_id)}};
    }
    body["service"] = std::move(service);
  }
  return Frame{.type = MessageType::hello_ack, .body = std::move(body), .payloads = {}};
}

WorkerHelloAck hello_ack_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::hello_ack, 0);
  const nlohmann::json& body = frame.body;
  WorkerHelloAck ack;
  ack.protocol = protocol_from_json(body, path);
  const bool accepted = required_bool(body, "accepted", path);
  const auto refusal = body.find("refusal");
  if (accepted == (refusal != body.end())) {
    throw ExecError(ExecErrorCode::invalid_value,
                    path + " carries a refusal exactly when accepted is false");
  }
  if (!accepted) {
    const std::string refusal_path = child_path(path, "refusal");
    require_object(*refusal, refusal_path);
    const std::string code = required_string(*refusal, "code", refusal_path);
    const std::optional<SessionRefusalCode> parsed = parse_session_refusal_code(code);
    // A newer worker may refuse for a reason this coordinator does not know;
    // it is still a refusal, reported with the worker's own words.
    ack.refusal = SessionRefusal{
        .code = parsed.value_or(SessionRefusalCode::protocol_mismatch),
        .message = (parsed ? "" : "(" + code + ") ") +
                   required_string(*refusal, "message", refusal_path)};
  }
  ack.host = host_facts_from_json(required_field(body, "host", path), child_path(path, "host"));
  const std::string memory_path = child_path(path, "memory");
  const nlohmann::json& memory = required_object(body, "memory", path);
  ack.memory.available_bytes = required_unsigned(memory, "available_bytes", memory_path);
  const std::string pressure = required_string(memory, "pressure", memory_path);
  const std::optional<MemoryPressure> parsed_pressure = parse_memory_pressure(pressure);
  if (!parsed_pressure) {
    throw ExecError(ExecErrorCode::invalid_value,
                    child_path(memory_path, "pressure") + " is not a pressure level");
  }
  ack.memory.pressure = *parsed_pressure;
  ack.memory_reserve_bytes = required_unsigned(memory, "reserve_bytes", memory_path);
  ack.disk_available_bytes = required_unsigned(required_object(body, "disk", path),
                                               "available_bytes", child_path(path, "disk"));
  const nlohmann::json& cache = required_object(body, "cache", path);
  ack.cache_blob_count = required_unsigned(cache, "blob_count", child_path(path, "cache"));
  ack.cache_total_bytes = required_unsigned(cache, "total_bytes", child_path(path, "cache"));
  ack.runtime_present = required_bool(body, "runtime_present", path);
  ack.runtimes = required_digest_array(body, "runtimes", path, true);
  ack.model_bundles = required_digest_array(body, "model_bundles", path, false);
  ack.active_sessions = required_unsigned(required_object(body, "sessions", path), "active",
                                          child_path(path, "sessions"));
  if (body.contains("agent_runtime_id")) {
    ack.agent_runtime_id = required_blake3_prefixed(body, "agent_runtime_id", path);
  }
  if (body.contains("worker_id")) {
    ack.worker_id = required_string(body, "worker_id", path);
  }
  if (const auto service = body.find("service"); service != body.end()) {
    const std::string service_path = child_path(path, "service");
    require_object(*service, service_path);
    ServiceUpdateState state;
    state.self_update = required_bool(*service, "self_update", service_path);
    if (service->contains("release_stamp")) {
      state.release_stamp = required_unsigned(*service, "release_stamp", service_path);
    }
    state.declined_runtimes =
        required_digest_array(*service, "declined_runtimes", service_path, true);
    if (const auto pending = service->find("pending"); pending != service->end()) {
      const std::string pending_path = child_path(service_path, "pending");
      require_object(*pending, pending_path);
      state.pending = PendingServiceSwitch{
          .runtime_id = required_blake3_prefixed(*pending, "runtime_id", pending_path),
          .release_stamp = required_unsigned(*pending, "release_stamp", pending_path),
          .bytes = required_unsigned(*pending, "bytes", pending_path)};
    }
    ack.service = std::move(state);
  }
  return ack;
}

std::optional<SessionRefusal> evaluate_hello(const CoordinatorHello& hello,
                                             const HostFacts& worker) {
  if (!protocol_compatible(kWorkerProtocolVersion, hello.protocol)) {
    return SessionRefusal{
        .code = SessionRefusalCode::protocol_mismatch,
        .message = "worker speaks protocol " + std::to_string(kWorkerProtocolVersion.major) +
                   "." + std::to_string(kWorkerProtocolVersion.minor) +
                   ", coordinator speaks " + std::to_string(hello.protocol.major) + "." +
                   std::to_string(hello.protocol.minor)};
  }
  if (hello.host.arch != worker.arch) {
    return SessionRefusal{.code = SessionRefusalCode::arch_mismatch,
                          .message = "worker is " + worker.arch + ", coordinator is " +
                                     hello.host.arch};
  }
  if (hello.host.os.product_version != worker.os.product_version) {
    return SessionRefusal{
        .code = SessionRefusalCode::os_mismatch,
        .message = "worker runs macOS " + worker.os.product_version + " (" + worker.os.build +
                   "), coordinator runs macOS " + hello.host.os.product_version + " (" +
                   hello.host.os.build + "); a worker must run the coordinator's macOS version"};
  }
  bool host_independent = false;
  try {
    const svp::models::ThreadPlan plan = svp::models::thread_plan_from_json(hello.thread_plan);
    host_independent = svp::models::thread_plan_problems(plan).empty() &&
                       svp::models::thread_plan_is_host_independent(plan);
  } catch (const std::exception& error) {
    return SessionRefusal{.code = SessionRefusalCode::thread_plan_host_dependent,
                          .message = std::string("thread plan is unusable: ") + error.what()};
  }
  if (!host_independent || !hello.thread_plan_host_independent) {
    return SessionRefusal{
        .code = SessionRefusalCode::thread_plan_host_dependent,
        .message = "the coordinator's thread plan leaves a thread count to the host, so task "
                   "output could depend on which Mac ran it"};
  }
  return std::nullopt;
}

}  // namespace svp::exec::worker
