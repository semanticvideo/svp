#include "svp/exec/task_spec.hpp"

#include "artifact_ref_json.hpp"
#include "json_fields.hpp"
#include "record_identifiers.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/models/model_id.hpp"

#include <string>

namespace svp::exec {
namespace {

constexpr std::string_view kRoot = "task_spec";

void require_identifier(const std::string& value, std::string_view name) {
  if (!detail::is_record_identifier(value)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec." + std::string(name) +
                        " must be a non-empty [A-Za-z0-9._-] identifier: `" +
                        value + "`");
  }
}

void validate_model_ref(const TaskModelRef& ref) {
  if (!svp::models::is_canonical_model_id(ref.model_id)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec.model_refs model_id is not canonical: `" +
                        ref.model_id + "`");
  }
  const auto parts = svp::models::parse_model_bundle_id(ref.model_bundle_id);
  if (!parts || parts->model_id != ref.model_id ||
      !blake3_hex(ref.bundle_blake3).starts_with(parts->hash_prefix)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec.model_refs model_bundle_id `" +
                        ref.model_bundle_id +
                        "` does not match its model_id and bundle_blake3");
  }
}

void validate_ordering(const TaskSpec& spec) {
  for (std::size_t index = 1; index < spec.depends_on.size(); ++index) {
    if (!(spec.depends_on[index - 1] < spec.depends_on[index])) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task_spec.depends_on must be strictly ascending");
    }
  }
  for (std::size_t index = 1; index < spec.model_refs.size(); ++index) {
    if (!(spec.model_refs[index - 1].model_id < spec.model_refs[index].model_id)) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task_spec.model_refs must be strictly ascending by "
                      "model_id");
    }
  }
}

nlohmann::json model_ref_to_json(const TaskModelRef& ref) {
  return nlohmann::json{{"bundle_blake3", blake3_hex(ref.bundle_blake3)},
                        {"model_bundle_id", ref.model_bundle_id},
                        {"model_id", ref.model_id}};
}

TaskModelRef model_ref_from_json(const nlohmann::json& value,
                                 std::string_view path) {
  detail::require_object(value, path);
  detail::reject_unknown_fields(
      value, {"bundle_blake3", "model_bundle_id", "model_id"}, path);
  return TaskModelRef{
      .model_id = detail::required_string(value, "model_id", path),
      .model_bundle_id = detail::required_string(value, "model_bundle_id", path),
      .bundle_blake3 = detail::required_blake3_hex(value, "bundle_blake3", path)};
}

TaskResources resources_from_json(const nlohmann::json& value,
                                  std::string_view path) {
  detail::reject_unknown_fields(
      value, {"est_cpu_threads", "est_peak_rss_mb", "est_seconds"}, path);
  return TaskResources{
      .est_peak_rss_mb = detail::required_unsigned(value, "est_peak_rss_mb", path),
      .est_cpu_threads = detail::required_unsigned(value, "est_cpu_threads", path),
      .est_seconds = detail::required_unsigned(value, "est_seconds", path)};
}

}  // namespace

void validate_task_spec(const TaskSpec& spec) {
  require_identifier(spec.build_session_id, "build_session_id");
  require_identifier(spec.task_id, "task_id");
  for (const std::string& dependency : spec.depends_on) {
    require_identifier(dependency, "depends_on[]");
  }
  if (!detail::is_lower_identifier(spec.task_type)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec.task_type must be a lowercase identifier: `" +
                        spec.task_type + "`");
  }
  if (spec.task_type_version == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec.task_type_version must be at least 1");
  }
  if (spec.resources.est_cpu_threads == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec.resources.est_cpu_threads must be at least 1");
  }
  validate_ordering(spec);
  for (const TaskModelRef& ref : spec.model_refs) {
    validate_model_ref(ref);
  }
  for (const auto& [name, ref] : spec.inputs) {
    if (!detail::is_lower_identifier(name)) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task_spec.inputs name must be a lowercase identifier: `" +
                          name + "`");
    }
    validate_artifact_ref(ref);
  }
  if (compute_parameters_blake3(spec.parameters) != spec.parameters_blake3) {
    throw ExecError(ExecErrorCode::digest_mismatch,
                    "task_spec.parameters_blake3 does not match the canonical "
                    "parameters");
  }
}

nlohmann::json task_spec_to_json(const TaskSpec& spec) {
  validate_task_spec(spec);
  nlohmann::json model_refs = nlohmann::json::array();
  for (const TaskModelRef& ref : spec.model_refs) {
    model_refs.push_back(model_ref_to_json(ref));
  }
  nlohmann::json inputs = nlohmann::json::object();
  for (const auto& [name, ref] : spec.inputs) {
    inputs[name] = artifact_ref_to_json(ref);
  }
  return nlohmann::json{
      {"build_session_id", spec.build_session_id},
      {"cache_key", blake3_prefixed(spec.cache_key)},
      {"depends_on", spec.depends_on},
      {"inputs", std::move(inputs)},
      {"model_refs", std::move(model_refs)},
      {"parameters", spec.parameters},
      {"parameters_blake3", blake3_hex(spec.parameters_blake3)},
      {"resources",
       {{"est_cpu_threads", spec.resources.est_cpu_threads},
        {"est_peak_rss_mb", spec.resources.est_peak_rss_mb},
        {"est_seconds", spec.resources.est_seconds}}},
      {"schema", std::string(kTaskSpecSchema)},
      {"task_id", spec.task_id},
      {"task_type", spec.task_type},
      {"task_type_version", spec.task_type_version}};
}

TaskSpec task_spec_from_json(const nlohmann::json& value) {
  detail::require_object(value, kRoot);
  detail::reject_unknown_fields(
      value,
      {"build_session_id", "cache_key", "depends_on", "inputs", "model_refs",
       "parameters", "parameters_blake3", "resources", "schema", "task_id",
       "task_type", "task_type_version"},
      kRoot);
  if (detail::required_string(value, "schema", kRoot) != kTaskSpecSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task_spec.schema must be \"" + std::string(kTaskSpecSchema) +
                        "\"");
  }

  TaskSpec spec;
  spec.build_session_id = detail::required_string(value, "build_session_id", kRoot);
  spec.task_id = detail::required_string(value, "task_id", kRoot);
  spec.task_type = detail::required_string(value, "task_type", kRoot);
  spec.task_type_version =
      detail::required_unsigned(value, "task_type_version", kRoot);

  const std::string depends_path = detail::child_path(kRoot, "depends_on");
  for (const nlohmann::json& dependency :
       detail::required_array(value, "depends_on", kRoot)) {
    if (!dependency.is_string()) {
      throw ExecError(ExecErrorCode::wrong_type,
                      depends_path + " entries must be strings");
    }
    spec.depends_on.push_back(dependency.get<std::string>());
  }

  const std::string models_path = detail::child_path(kRoot, "model_refs[]");
  for (const nlohmann::json& ref :
       detail::required_array(value, "model_refs", kRoot)) {
    spec.model_refs.push_back(model_ref_from_json(ref, models_path));
  }

  const std::string inputs_path = detail::child_path(kRoot, "inputs");
  const nlohmann::json& inputs = detail::required_object(value, "inputs", kRoot);
  for (auto iterator = inputs.begin(); iterator != inputs.end(); ++iterator) {
    spec.inputs.emplace(iterator.key(),
                        detail::artifact_ref_from_json_at(
                            iterator.value(),
                            detail::child_path(inputs_path, iterator.key())));
  }

  spec.parameters = detail::required_object(value, "parameters", kRoot);
  spec.parameters_blake3 =
      detail::required_blake3_hex(value, "parameters_blake3", kRoot);
  spec.cache_key = detail::required_blake3_prefixed(value, "cache_key", kRoot);
  spec.resources = resources_from_json(
      detail::required_object(value, "resources", kRoot),
      detail::child_path(kRoot, "resources"));

  validate_task_spec(spec);
  return spec;
}

std::string encode_task_spec(const TaskSpec& spec) {
  return encode_canonical_json(task_spec_to_json(spec));
}

TaskSpec decode_task_spec(std::string_view bytes) {
  return task_spec_from_json(decode_canonical_json(bytes));
}

}  // namespace svp::exec
