#include "engine/stage_task_registry.hpp"

#include "engine/stage_task_products.hpp"
#include "engine/stage_tasks.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/output_digest.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace svp::builder::engine {
namespace {

// Whole-stage task parameters: the build inputs digest and the staging scope
// (stage_task_plan.cpp). Anything else is a spec this runtime did not plan.
std::optional<std::string> validate_stage_parameters(const nlohmann::json& parameters) {
  if (!parameters.is_object() || parameters.size() != 2 ||
      !parameters.contains("build_inputs_blake3") ||
      !parameters["build_inputs_blake3"].is_string() ||
      !parameters.contains("staging_scope") || !parameters["staging_scope"].is_array()) {
    return "expected {build_inputs_blake3, staging_scope}";
  }
  return std::nullopt;
}

// TaskTypeRegistry validates a result before the attempt runner stamps the
// executing session into it, so results start with this placeholder.
constexpr const char* kUnstampedWorkerSession = "ws_unstamped";

svp::exec::TaskResult stage_exit_result(const svp::exec::TaskSpec& spec,
                                        const StageExitError& error) {
  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = 1;
  result.status = svp::exec::TaskStatus::failed;
  result.execution.worker_session_id = kUnstampedWorkerSession;
  result.output_digest = svp::exec::compute_output_digest({});
  result.error = svp::exec::TaskError{.code = kStageExitStatusErrorCode,
                                      .message = error.what(),
                                      .retryable = false};
  return result;
}

svp::exec::TaskResult run_and_capture(StageTaskKind kind, const StagingScope& scope,
                                      const svp::exec::TaskSpec& spec,
                                      const svp::exec::CancellationToken& cancellation,
                                      const StageTaskEnvironment& environment,
                                      StageOutputAccess& outputs, StageExitRecord& exits) {
  svp::exec::throw_if_cancelled(cancellation, spec.task_id + " start");
  StageStates states;
  try {
    states = run_stage_task(kind, environment);
  } catch (const StageExitError& error) {
    exits.record(error.exit_code());
    return stage_exit_result(spec, error);
  }
  // Stage code does not check cancellation itself; a result produced after
  // cancellation is never committed.
  svp::exec::throw_if_cancelled(cancellation, spec.task_id + " end");

  StageTaskProducts products{.staging = capture_staging_scope(environment.staging_dir, scope),
                             .states = std::move(states)};
  const std::size_t staged_entries = products.staging.size();
  EncodedStageProducts encoded = encode_stage_products(std::move(products));

  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = 1;
  result.status = svp::exec::TaskStatus::succeeded;
  result.execution.worker_session_id = kUnstampedWorkerSession;
  result.outputs = std::move(encoded.outputs);
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  result.diagnostics = {{"staging_entries", staged_entries}};
  outputs.stage(spec.task_id, std::move(encoded.payloads));
  return result;
}

}  // namespace

void StageExitRecord::record(int exit_code) {
  const std::lock_guard lock(mutex_);
  if (!exit_code_) {
    exit_code_ = exit_code;
  }
}

std::optional<int> StageExitRecord::exit_code() const {
  const std::lock_guard lock(mutex_);
  return exit_code_;
}

StageStates run_stage_task(StageTaskKind kind, const StageTaskEnvironment& environment) {
  switch (kind) {
    case StageTaskKind::inventory: return run_inventory_task(environment);
    case StageTaskKind::color: return run_color_task(environment);
    case StageTaskKind::vision_plan: return run_vision_plan_task(environment);
    case StageTaskKind::foundation_ocr: return run_foundation_ocr_task(environment);
    case StageTaskKind::audio_extract: return run_audio_extract_task(environment);
    case StageTaskKind::audio_transcribe: return run_audio_transcribe_task(environment);
    case StageTaskKind::canonical_frames: return run_canonical_frames_task(environment);
    case StageTaskKind::depth: return run_depth_task(environment);
    case StageTaskKind::ocr: return run_ocr_task(environment);
    case StageTaskKind::text_embeddings: return run_text_embeddings_task(environment);
    case StageTaskKind::tracking: return run_tracking_task(environment);
    case StageTaskKind::entities: return run_entities_task(environment);
    case StageTaskKind::relationships: return run_relationships_task(environment);
    case StageTaskKind::index: return run_index_task(environment);
    case StageTaskKind::validation: return run_validation_task(environment);
    case StageTaskKind::package_write: return run_package_write_task(environment);
    case StageTaskKind::media_binding: return run_media_binding_task(environment);
    case StageTaskKind::svpi_write: return run_svpi_write_task(environment);
  }
  throw std::logic_error("unknown stage task kind");
}

void register_stage_task_types(svp::exec::TaskTypeRegistry& registry,
                               const std::vector<PlannedStageTask>& tasks,
                               const StageTaskEnvironment& environment,
                               StageOutputAccess& outputs, StageExitRecord& exits) {
  for (const PlannedStageTask& task : tasks) {
    registry.register_type(svp::exec::TaskTypeDefinition{
        .name = std::string(stage_task_type(task.kind)),
        .version = kStageTaskTypeVersion,
        .validate_parameters = validate_stage_parameters,
        .execute = [kind = task.kind, scope = task.scope, &environment, &outputs, &exits](
                       const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs&,
                       const svp::exec::CancellationToken& cancellation) {
          return run_and_capture(kind, scope, spec, cancellation, environment, outputs,
                                 exits);
        }});
  }
}

}  // namespace svp::builder::engine
