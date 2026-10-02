#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/vision/ocr_frame_batch.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
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
                           .message = "ocr.frame_batch: " + std::move(message),
                           .retryable = retryable};
  return result;
}

const TaskModelRef* find_ref(const TaskSpec& spec, std::string_view model_id) {
  const auto found = std::find_if(
      spec.model_refs.begin(), spec.model_refs.end(),
      [&](const TaskModelRef& ref) { return ref.model_id == model_id; });
  return found == spec.model_refs.end() ? nullptr : &*found;
}

// nullopt when the loaded bundles are the ones the coordinator named.
std::optional<std::string> model_identity_mismatch(const PpOcrModelInfo& loaded,
                                                   const TaskModelRef& det,
                                                   const TaskModelRef& rec) {
  if (loaded.det_model_id != det.model_id ||
      loaded.det_bundle_blake3 != svp::exec::blake3_hex(det.bundle_blake3)) {
    return "detector bundle " + loaded.det_model_id + "@" + loaded.det_bundle_blake3 +
           " is not " + det.model_bundle_id;
  }
  if (loaded.rec_model_id != rec.model_id ||
      loaded.rec_bundle_blake3 != svp::exec::blake3_hex(rec.bundle_blake3)) {
    return "recognizer bundle " + loaded.rec_model_id + "@" + loaded.rec_bundle_blake3 +
           " is not " + rec.model_bundle_id;
  }
  return std::nullopt;
}

std::vector<std::byte> to_bytes(const std::string& text) {
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](char character) { return static_cast<std::byte>(character); });
  return bytes;
}

TaskResult succeeded(const TaskSpec& spec, const OcrFrameBatchWorkerEnvironment& environment,
                     const std::vector<OcrSampleDetections>& samples) {
  std::size_t decoded = 0;
  std::size_t detections = 0;
  for (const OcrSampleDetections& sample : samples) {
    if (sample.status != OcrSampleStatus::decode_missed &&
        sample.status != OcrSampleStatus::not_started) {
      ++decoded;
    }
    detections += sample.detections.size();
  }
  TaskResult result = unstamped_result(spec);
  result.status = TaskStatus::succeeded;
  result.outputs = {environment.write_output(
      to_bytes(encode_ocr_sample_detections_jsonl(samples)),
      std::string(kOcrFrameDetectionsMediaType), std::string(kOcrFrameDetectionsRole))};
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  result.diagnostics = {
      {"decoded_samples", decoded},
      {"detections", detections},
      {"samples", samples.size()},
  };
  return result;
}

// OCR could not start for this batch (ffmpeg cannot decode at all, or
// PP-OCR did not load). On a worker it is a retryable failure, so another
// Mac takes the batch. On the coordinator every sample is recorded as
// not_started, and the OCR stage then runs as it does without batches,
// reporting the same blocker a build without batches reports.
TaskResult could_not_start(const TaskSpec& spec, const OcrFrameBatchParameters& parameters,
                           const OcrFrameBatchWorkerEnvironment& environment,
                           std::string code, std::string reason) {
  if (!environment.record_start_failures) {
    return failed(spec, std::move(code), std::move(reason), true);
  }
  std::vector<OcrSampleDetections> samples;
  samples.reserve(parameters.samples.size());
  for (const OcrSample& sample : parameters.samples) {
    samples.push_back(OcrSampleDetections{.sample_ordinal = sample.ordinal,
                                          .timestamp_us = sample.timestamp_us,
                                          .status = OcrSampleStatus::not_started,
                                          .error = reason});
  }
  return succeeded(spec, environment, samples);
}

TaskResult execute(const TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                   const svp::exec::CancellationToken& cancellation,
                   const OcrFrameBatchWorkerEnvironment& environment,
                   PpOcrSessionPool& sessions) {
  svp::exec::throw_if_cancelled(cancellation, "ocr.frame_batch start");
  const OcrFrameBatchParameters parameters =
      ocr_frame_batch_parameters_from_json(spec.parameters);

  if (spec.inputs.size() != 1 ||
      !spec.inputs.contains(std::string(kOcrFrameBatchSourceInput))) {
    return failed(spec, "invalid_inputs",
                  "inputs must be exactly `" + std::string(kOcrFrameBatchSourceInput) + "`",
                  false);
  }
  const TaskModelRef* det = find_ref(spec, parameters.pp_ocr.detector_model_id);
  const TaskModelRef* rec = find_ref(spec, parameters.pp_ocr.recognizer_model_id);
  if (det == nullptr || rec == nullptr || spec.model_refs.size() != 2) {
    return failed(spec, "invalid_model_refs",
                  "model_refs must name exactly the detector and recognizer", false);
  }

  PpOcrOptions pp_ocr = parameters.pp_ocr;
  if (environment.model_cache_for) {
    try {
      pp_ocr.model_cache_root = environment.model_cache_for(spec);
    } catch (const std::exception& error) {
      return failed(spec, "model_unavailable", error.what(), true);
    }
  } else {
    pp_ocr.model_cache_root = environment.model_cache_root;
  }

  const std::optional<std::string> decoder =
      cached_ffmpeg_build_identity(environment.ffmpeg_path);
  if (!decoder) {
    return could_not_start(spec, parameters, environment, "decode_unavailable",
                           "cannot run ffmpeg at " + environment.ffmpeg_path.string());
  }
  if (*decoder != parameters.ffmpeg_build) {
    return failed(spec, "decoder_mismatch",
                  "ffmpeg at " + environment.ffmpeg_path.string() + " is build " + *decoder +
                      ", the coordinator decodes with " + parameters.ffmpeg_build,
                  true);
  }

  const std::unique_ptr<PpOcrSessionPool::Lease> lease = sessions.acquire(pp_ocr);
  const PpOcrSession& session = lease->session();
  if (!session.available) {
    return could_not_start(spec, parameters, environment, "ocr_unavailable", session.blocker);
  }
  if (const auto mismatch = model_identity_mismatch(session.model_info, *det, *rec)) {
    return failed(spec, "model_mismatch", *mismatch, true);
  }

  const OcrFrameBatchRequest request{
      .source_path = inputs.at(std::string(kOcrFrameBatchSourceInput)).path,
      .ffmpeg_path = environment.ffmpeg_path,
      .frame_width = parameters.frame_width,
      .frame_height = parameters.frame_height,
      .samples = parameters.samples,
  };
  OcrFrameBatchHooks hooks;
  hooks.before_sample = [&cancellation] {
    svp::exec::throw_if_cancelled(cancellation, "ocr.frame_batch between frames");
  };
  const OcrFrameBatchOutcome outcome = run_ocr_frame_batch(session, pp_ocr, request, hooks);
  if (!outcome.decoding_attempted) {
    return could_not_start(spec, parameters, environment, "decode_unavailable",
                           outcome.skipped_reason);
  }
  svp::exec::throw_if_cancelled(cancellation, "ocr.frame_batch before output");
  return succeeded(spec, environment, outcome.samples);
}

}  // namespace

void register_ocr_frame_batch_task(svp::exec::TaskTypeRegistry& registry,
                                   OcrFrameBatchWorkerEnvironment environment,
                                   std::shared_ptr<PpOcrSessionPool> sessions) {
  if (!sessions) {
    sessions = std::make_shared<PpOcrSessionPool>();
  }
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kOcrFrameBatchTaskType),
      .version = kOcrFrameBatchTaskTypeVersion,
      .validate_parameters = validate_ocr_frame_batch_parameters,
      .execute = [environment = std::move(environment), sessions](
                     const TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                     const svp::exec::CancellationToken& cancellation) {
        return execute(spec, inputs, cancellation, environment, *sessions);
      }});
}

}  // namespace svp::vision::tasks
