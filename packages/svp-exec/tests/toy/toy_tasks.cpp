#include "toy_tasks.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/parameters_digest.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>
#include <utility>

namespace svp::exec::test {
namespace {

constexpr std::array kAllFaults = {
    ToyFault::none,           ToyFault::crash,          ToyFault::corrupt_in_transit,
    ToyFault::hang,           ToyFault::nondeterministic, ToyFault::fail_retryable,
    ToyFault::fail_permanent,
};

std::optional<ToyFault> parse_fault(std::string_view name) {
  for (const ToyFault fault : kAllFaults) {
    if (toy_fault_name(fault) == name) {
      return fault;
    }
  }
  return std::nullopt;
}

std::optional<std::string> validate_parameters(const nlohmann::json& parameters) {
  for (const auto& [name, value] : parameters.items()) {
    if (name == "seed" || name == "sleep_ms") {
      if (!value.is_number_unsigned()) {
        return name + " must be an unsigned integer";
      }
    } else if (name == "fault") {
      if (!value.is_string() || !parse_fault(value.get<std::string>())) {
        return "unknown fault";
      }
    } else if (name == "once_marker") {
      if (!value.is_string()) {
        return "once_marker must be a string";
      }
    } else {
      return "unknown parameter `" + name + "`";
    }
  }
  if (!parameters.contains("seed")) {
    return "seed is required";
  }
  return std::nullopt;
}

// A fault fires every time, or with once_marker only the first time.
bool fault_fires(const nlohmann::json& parameters) {
  if (!parameters.contains("once_marker")) {
    return true;
  }
  const std::filesystem::path marker = parameters.at("once_marker").get<std::string>();
  if (std::filesystem::exists(marker)) {
    return false;
  }
  std::ofstream(marker) << "fired\n";
  return true;
}

void sleep_jitter(std::chrono::microseconds max_jitter) {
  if (max_jitter.count() <= 0) {
    return;
  }
  thread_local std::mt19937_64 generator{std::random_device{}()};
  std::uniform_int_distribution<std::int64_t> distribution(0, max_jitter.count());
  std::this_thread::sleep_for(std::chrono::microseconds(distribution(generator)));
}

std::vector<std::byte> to_bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](char character) { return static_cast<std::byte>(character); });
  return bytes;
}

TaskResult base_result(const TaskSpec& spec) {
  TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = 1;
  result.execution.worker_session_id = "ws_toy";
  return result;
}

TaskResult failed_result(const TaskSpec& spec, bool retryable) {
  TaskResult result = base_result(spec);
  result.status = TaskStatus::failed;
  result.output_digest = compute_output_digest({});
  result.error = TaskError{.code = "toy_failure",
                           .message = "toy task asked to fail",
                           .retryable = retryable};
  return result;
}

TaskResult execute(const TaskSpec& spec, InMemoryArtifactStore& store,
                   const ToyTaskOptions& options) {
  const nlohmann::json& parameters = spec.parameters;
  sleep_jitter(options.max_jitter);
  if (const auto sleep_ms = parameters.find("sleep_ms"); sleep_ms != parameters.end()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms->get<std::uint64_t>()));
  }

  ToyFault fault = ToyFault::none;
  if (options.faults == ToyFaults::honoured && parameters.contains("fault") &&
      fault_fires(parameters)) {
    fault = *parse_fault(parameters.at("fault").get<std::string>());
  }

  std::string text = toy_expected_output(spec.task_id, parameters.at("seed").get<std::uint64_t>());
  TaskResult result = base_result(spec);
  switch (fault) {
    case ToyFault::crash:
      std::_Exit(kToyCrashExitStatus);
    case ToyFault::hang:
      std::raise(SIGSTOP);
      break;
    case ToyFault::fail_retryable:
      return failed_result(spec, true);
    case ToyFault::fail_permanent:
      return failed_result(spec, false);
    case ToyFault::nondeterministic:
      text += " nonce=" + std::to_string(std::random_device{}());
      break;
    case ToyFault::corrupt_in_transit:
      result.diagnostics[std::string(kToyCorruptInTransit)] = true;
      break;
    case ToyFault::none:
      break;
  }
  result.outputs = {store.put(to_bytes(text), "text/plain", "toy_output")};
  result.output_digest = compute_output_digest(result.outputs);
  return result;
}

}  // namespace

std::string_view toy_fault_name(ToyFault fault) {
  switch (fault) {
    case ToyFault::none:
      return "none";
    case ToyFault::crash:
      return "crash";
    case ToyFault::corrupt_in_transit:
      return "corrupt_in_transit";
    case ToyFault::hang:
      return "hang";
    case ToyFault::nondeterministic:
      return "nondeterministic";
    case ToyFault::fail_retryable:
      return "fail_retryable";
    case ToyFault::fail_permanent:
      return "fail_permanent";
  }
  return "unknown";
}

void register_toy_tasks(TaskTypeRegistry& registry, InMemoryArtifactStore& store,
                        ToyTaskOptions options) {
  registry.register_type(TaskTypeDefinition{
      .name = std::string(kToyTaskType),
      .version = 1,
      .validate_parameters = validate_parameters,
      .execute = [&store, options](const TaskSpec& spec, const ResolvedInputs&) {
        return execute(spec, store, options);
      }});
}

std::string toy_expected_output(std::string_view task_id, std::uint64_t seed) {
  const std::string material = std::string(task_id) + "#" + std::to_string(seed);
  return "toy.digest " + std::string(task_id) + " seed=" + std::to_string(seed) +
         " b3=" + blake3_hex(blake3_digest(material)) + "\n";
}

TaskNode make_toy_node(const ToyTask& task) {
  nlohmann::json parameters{{"seed", task.seed}};
  if (task.fault != ToyFault::none) {
    parameters["fault"] = std::string(toy_fault_name(task.fault));
  }
  if (task.once_marker) {
    parameters["once_marker"] = task.once_marker->string();
  }
  if (task.sleep_ms != 0) {
    parameters["sleep_ms"] = task.sleep_ms;
  }

  TaskSpec spec;
  spec.build_session_id = std::string(kToyBuildSession);
  spec.task_id = task.task_id;
  spec.task_type = std::string(kToyTaskType);
  spec.task_type_version = 1;
  spec.depends_on = task.depends_on;
  std::sort(spec.depends_on.begin(), spec.depends_on.end());
  spec.parameters = parameters;
  spec.parameters_blake3 = compute_parameters_blake3(parameters);
  spec.cache_key = blake3_digest(task.task_id);
  spec.resources = TaskResources{
      .est_peak_rss_mb = 1, .est_cpu_threads = 1, .est_seconds = task.est_seconds};
  return TaskNode{.spec = std::move(spec), .order_key = task.order_key};
}

}  // namespace svp::exec::test
