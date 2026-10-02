#include "svp/vision/tasks/track_window_task.hpp"

#include "svp/exec/output_digest.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"
#include "svp/vision/visual_entity_window_codec.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace svp::vision::tasks {
namespace {

using svp::exec::TaskError;
using svp::exec::TaskModelRef;
using svp::exec::TaskResult;
using svp::exec::TaskSpec;
using svp::exec::TaskStatus;

// TaskTypeRegistry::execute validates the result before run_task_attempt
// stamps the real attempt number and worker session over these.
constexpr std::uint64_t kUnstampedAttempt = 1;
constexpr std::string_view kUnstampedWorkerSession = "ws_unstamped";

TaskResult unstamped_result(const TaskSpec& spec) {
  TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = kUnstampedAttempt;
  result.execution.worker_session_id = std::string(kUnstampedWorkerSession);
  return result;
}

TaskResult failed(const TaskSpec& spec, std::string code, std::string message,
                  bool retryable) {
  TaskResult result = unstamped_result(spec);
  result.status = TaskStatus::failed;
  result.output_digest = svp::exec::compute_output_digest({});
  result.error = TaskError{.code = std::move(code),
                           .message = "track.window: " + std::move(message),
                           .retryable = retryable};
  return result;
}

const TaskModelRef* find_ref(const TaskSpec& spec, std::string_view model_id) {
  const auto found =
      std::find_if(spec.model_refs.begin(), spec.model_refs.end(),
                   [&](const TaskModelRef& ref) { return ref.model_id == model_id; });
  return found == spec.model_refs.end() ? nullptr : &*found;
}

// nullopt when the loaded detector and depth bundles are the ones the
// coordinator named (the embedding runtime records no bundle identity; the
// model view or cache it loaded from holds exactly the named bundle).
std::optional<std::string> model_identity_mismatch(const VisualEntityWindowRuntimes& runtimes,
                                                   const TaskModelRef& detector,
                                                   const TaskModelRef& depth) {
  const std::string loaded_detector =
      runtimes.detector.model_identity.value("model_bundle_id", std::string());
  if (loaded_detector != detector.model_bundle_id) {
    return "detector bundle " + loaded_detector + " is not " + detector.model_bundle_id;
  }
  if (runtimes.depth.model_bundle_id != depth.model_bundle_id) {
    return "depth bundle " + runtimes.depth.model_bundle_id + " is not " +
           depth.model_bundle_id;
  }
  return std::nullopt;
}

std::string runtime_blockers(const VisualEntityWindowRuntimes& runtimes) {
  std::string reason;
  const auto add = [&reason](const std::string& part) {
    reason += (reason.empty() ? "" : "; ") + part;
  };
  if (!runtimes.detector.session) add("objectness detector: " + runtimes.detector.blocker);
  if (!runtimes.depth.session) add("depth: " + runtimes.depth.blocker);
  if (!runtimes.embedding.session) add("visual embedding: " + runtimes.embedding.limitations_note);
  return reason;
}

// A window whose outcome records anything that went wrong: frames that did
// not decode, a decoder that exited badly, or an error from the detector,
// depth, or tracker. Those can come from one Mac's conditions (memory,
// a crashed decoder) rather than from the window.
std::optional<std::string> trouble(const VisualEntityWindowOutcome& outcome) {
  if (!outcome.failures.empty()) {
    return outcome.failures.front().component + ": " + outcome.failures.front().message;
  }
  if (outcome.status != VisualEntityWindowStatus::tracked) {
    return "the window was not tracked";
  }
  if (outcome.frames_missed != 0) {
    return std::to_string(outcome.frames_missed) + " frame(s) did not decode";
  }
  return std::nullopt;
}

TaskResult succeeded(const TaskSpec& spec, const TrackWindowWorkerEnvironment& environment,
                     const VisualEntityWindowOutcome& outcome) {
  const std::vector<std::uint8_t> encoded = encode_visual_entity_window_outcome(outcome);
  std::vector<std::byte> bytes(encoded.size());
  std::memcpy(bytes.data(), encoded.data(), encoded.size());
  TaskResult result = unstamped_result(spec);
  result.status = TaskStatus::succeeded;
  result.outputs = {environment.write_output(bytes, std::string(kTrackWindowOutcomeMediaType),
                                             std::string(kTrackWindowOutcomeRole))};
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  result.diagnostics = {
      {"frames_decoded", outcome.frames_decoded},
      {"regions", outcome.tracker_result.regions.size()},
      {"failures", outcome.failures.size()},
  };
  return result;
}

// The window cannot start here (ffmpeg cannot run, or a runtime did not
// load). On a worker it is a retryable failure, so another Mac takes the
// window. On the coordinator the outcome is not_started, and the tracking
// stage then runs as it does without window tasks.
TaskResult could_not_start(const TaskSpec& spec, const TrackWindowWorkerEnvironment& environment,
                           std::string code, std::string reason) {
  if (!environment.record_start_failures) {
    return failed(spec, std::move(code), std::move(reason), true);
  }
  VisualEntityWindowOutcome outcome;
  outcome.status = VisualEntityWindowStatus::not_started;
  outcome.failures.push_back({code, reason});
  return succeeded(spec, environment, outcome);
}

TaskResult execute(const TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                   const svp::exec::CancellationToken& cancellation,
                   const TrackWindowWorkerEnvironment& environment,
                   TrackWindowRuntimePool& pool) {
  svp::exec::throw_if_cancelled(cancellation, "track.window start");
  const TrackWindowParameters parameters = track_window_parameters_from_json(spec.parameters);

  if (spec.inputs.size() != 1 || !spec.inputs.contains(std::string(kTrackWindowSourceInput))) {
    return failed(spec, "invalid_inputs",
                  "inputs must be exactly `" + std::string(kTrackWindowSourceInput) + "`", false);
  }
  const TaskModelRef* detector = find_ref(spec, parameters.detector.model_id);
  const TaskModelRef* depth = find_ref(spec, track_window_depth_model_id());
  const TaskModelRef* embedding = find_ref(spec, parameters.embedding_model_id);
  const std::set<std::string> named{parameters.detector.model_id, track_window_depth_model_id(),
                                    parameters.embedding_model_id};
  if (detector == nullptr || depth == nullptr || embedding == nullptr ||
      spec.model_refs.size() != named.size()) {
    return failed(spec, "invalid_model_refs",
                  "model_refs must name exactly the detector, depth, and embedding models",
                  false);
  }

  std::filesystem::path model_cache_root = environment.model_cache_root;
  if (environment.model_cache_for) {
    try {
      model_cache_root = environment.model_cache_for(spec);
    } catch (const std::exception& error) {
      return failed(spec, "model_unavailable", error.what(), true);
    }
  }

  const std::optional<std::string> decoder = cached_ffmpeg_build_identity(environment.ffmpeg_path);
  if (!decoder) {
    return could_not_start(spec, environment, "decode_unavailable",
                           "cannot run ffmpeg at " + environment.ffmpeg_path.string());
  }
  if (*decoder != parameters.ffmpeg_build) {
    return failed(spec, "decoder_mismatch",
                  "ffmpeg at " + environment.ffmpeg_path.string() + " is build " + *decoder +
                      ", the coordinator decodes with " + parameters.ffmpeg_build,
                  true);
  }

  const VisualEntityPipelineOptions options = track_window_pipeline_options(parameters);
  const std::unique_ptr<TrackWindowRuntimePool::Lease> lease =
      pool.acquire(model_cache_root, options);
  VisualEntityWindowRuntimes& runtimes = lease->runtimes();
  if (!visual_entity_window_runtimes_complete(runtimes)) {
    return could_not_start(spec, environment, "tracking_unavailable", runtime_blockers(runtimes));
  }
  if (const auto mismatch = model_identity_mismatch(runtimes, *detector, *depth)) {
    return failed(spec, "model_mismatch", *mismatch, true);
  }

  VisualEntityWindowFrameIdentity identity;
  identity.planned_frame_ids = parameters.frame_ids;
  identity.planned_frame_indices.assign(parameters.frame_indices.begin(),
                                        parameters.frame_indices.end());
  const VisualEntityWindowOutcome outcome = run_visual_entity_window(
      track_window_request(parameters, inputs.at(std::string(kTrackWindowSourceInput)).path,
                           environment.ffmpeg_path),
      runtimes, identity, [&cancellation] {
        svp::exec::throw_if_cancelled(cancellation, "track.window between frames");
      });
  svp::exec::throw_if_cancelled(cancellation, "track.window before output");
  if (!environment.record_start_failures) {
    // On a worker only a clean window is a result: anything that went wrong
    // is retried on another Mac and, if it goes wrong everywhere, recorded by
    // the coordinator exactly as a local build records it. So a package never
    // depends on which Mac ran a window.
    if (const std::optional<std::string> problem = trouble(outcome)) {
      return failed(spec, "window_failed_here", *problem, true);
    }
  }
  return succeeded(spec, environment, outcome);
}

}  // namespace

void register_track_window_task(svp::exec::TaskTypeRegistry& registry,
                                TrackWindowWorkerEnvironment environment,
                                std::shared_ptr<TrackWindowRuntimePool> runtimes) {
  if (!runtimes) {
    runtimes = std::make_shared<TrackWindowRuntimePool>();
  }
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kTrackWindowTaskType),
      .version = kTrackWindowTaskTypeVersion,
      .validate_parameters = validate_track_window_parameters,
      .execute = [environment = std::move(environment), runtimes](
                     const TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                     const svp::exec::CancellationToken& cancellation) {
        return execute(spec, inputs, cancellation, environment, *runtimes);
      }});
}

}  // namespace svp::vision::tasks
