#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

inline constexpr std::string_view kTaskSpecSchema = "svp-task-spec-v1";

// Canonical model identity a task depends on (RC2 §20.3 requires model_id,
// model_bundle_id, and bundle_blake3 in cache keys).
struct TaskModelRef {
  std::string model_id;
  std::string model_bundle_id;
  Blake3Digest bundle_blake3{};

  bool operator==(const TaskModelRef&) const = default;
};

// Scheduler estimates from the task type's resource model (plan §3.5, §4.4).
// All integers: RSS in MiB, whole CPU threads, whole seconds.
struct TaskResources {
  std::uint64_t est_peak_rss_mb = 0;
  std::uint64_t est_cpu_threads = 0;
  std::uint64_t est_seconds = 0;

  bool operator==(const TaskResources&) const = default;
};

// One schedulable unit of work (plan §4.2, RC2 §20.2). Canonical JSON form:
//   {"build_session_id", "cache_key":"b3:<hex>", "depends_on":[...],
//    "inputs":{name: ArtifactRef}, "model_refs":[...], "parameters":{...},
//    "parameters_blake3":"<hex>", "resources":{...},
//    "schema":"svp-task-spec-v1", "task_id", "task_type",
//    "task_type_version"}
// Every field is required; unknown fields are rejected.
//
// Ordering rules that keep the canonical bytes independent of how the spec was
// assembled:
//   * depends_on is strictly ascending (sorted, no duplicates);
//   * model_refs is strictly ascending by model_id;
//   * inputs is a JSON object, so names are sorted by canonical encoding.
struct TaskSpec {
  std::string build_session_id;
  std::string task_id;
  std::string task_type;
  std::uint64_t task_type_version = 0;
  std::vector<std::string> depends_on;
  std::vector<TaskModelRef> model_refs;
  std::map<std::string, ArtifactRef> inputs;
  nlohmann::json parameters = nlohmann::json::object();
  Blake3Digest parameters_blake3{};
  Blake3Digest cache_key{};
  TaskResources resources;

  bool operator==(const TaskSpec&) const = default;
};

// Semantic validation shared by encode and decode: identifier rules, ordering
// rules, model identity consistency, task_type_version >= 1,
// est_cpu_threads >= 1, finite parameters, and
// parameters_blake3 == compute_parameters_blake3(parameters)
// (ExecError(digest_mismatch) when it differs).
void validate_task_spec(const TaskSpec& spec);

[[nodiscard]] nlohmann::json task_spec_to_json(const TaskSpec& spec);
[[nodiscard]] TaskSpec task_spec_from_json(const nlohmann::json& value);

// Canonical bytes of the spec, and the strict inverse (non-canonical input is
// rejected).
[[nodiscard]] std::string encode_task_spec(const TaskSpec& spec);
[[nodiscard]] TaskSpec decode_task_spec(std::string_view bytes);

}  // namespace svp::exec
