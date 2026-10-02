#include "svp/vision/tasks/ocr_crop_batch_task.hpp"

#include "task_results.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/vision/evidence_crop_work.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>

namespace svp::vision::tasks {
namespace {

using svp::exec::TaskModelRef;
using svp::exec::TaskResult;
using svp::exec::TaskSpec;

TaskResult failed(const TaskSpec& spec, std::string code, std::string message, bool retryable) {
  return detail::failed_result(spec, kOcrCropBatchTaskType, std::move(code), std::move(message),
                               retryable);
}

const TaskModelRef* find_ref(const TaskSpec& spec, std::string_view model_id) {
  const auto found = std::find_if(spec.model_refs.begin(), spec.model_refs.end(),
                                  [&](const TaskModelRef& ref) { return ref.model_id == model_id; });
  return found == spec.model_refs.end() ? nullptr : &*found;
}

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

nlohmann::json not_extracted(const EvidenceCropJob& job) {
  return nlohmann::json{{"extracted", false}, {"ordinal", job.ordinal}};
}

TaskResult succeeded(const TaskSpec& spec, const DispatchedTaskEnvironment& environment,
                     const std::vector<nlohmann::json>& records,
                     const std::vector<std::byte>& images, std::size_t extracted) {
  return detail::succeeded_result(spec, environment.write_output, records, images,
                                  kOcrCropRecordsRole, kOcrCropImagesRole,
                                  {{"extracted", extracted}, {"jobs", records.size()}});
}

// The crops cannot be made here (ffmpeg cannot run, PP-OCR did not load). On
// a worker another Mac takes the batch; on the coordinator every job is
// reported not extracted and the stage makes each crop itself.
TaskResult could_not_start(const TaskSpec& spec, const OcrCropBatchParameters& parameters,
                           const DispatchedTaskEnvironment& environment, std::string code,
                           std::string reason) {
  if (!environment.record_start_failures) {
    return failed(spec, std::move(code), std::move(reason), true);
  }
  std::vector<nlohmann::json> records;
  for (const EvidenceCropJob& job : parameters.jobs) {
    records.push_back(not_extracted(job));
  }
  return succeeded(spec, environment, records, {}, 0);
}

std::string scratch_name(const TaskSpec& spec, const EvidenceCropJob& job) {
  // Attempts of one task may overlap in one runtime (a cancelled attempt
  // still finishing), so every scratch file name is unique to its call.
  static std::atomic<std::uint64_t> next{0};
  return spec.task_id + "." + std::to_string(job.ordinal) + "." +
         std::to_string(next.fetch_add(1)) + "." + job.image_format;
}

TaskResult execute(const TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                   const svp::exec::CancellationToken& cancellation,
                   const DispatchedTaskEnvironment& environment, PpOcrSessionPool& sessions) {
  svp::exec::throw_if_cancelled(cancellation, "ocr.crop_batch start");
  const OcrCropBatchParameters parameters = ocr_crop_batch_parameters_from_json(spec.parameters);
  if (spec.inputs.size() != 1 || !spec.inputs.contains(std::string(kOcrCropBatchSourceInput))) {
    return failed(spec, "invalid_inputs",
                  "inputs must be exactly `" + std::string(kOcrCropBatchSourceInput) + "`", false);
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

  const std::optional<std::string> decoder = cached_ffmpeg_build_identity(environment.ffmpeg_path);
  if (!decoder) {
    return could_not_start(spec, parameters, environment, "decode_unavailable",
                           "cannot run ffmpeg at " + environment.ffmpeg_path.string());
  }
  if (*decoder != parameters.ffmpeg_build) {
    return failed(spec, "decoder_mismatch",
                  "ffmpeg at " + environment.ffmpeg_path.string() + " is build " + *decoder +
                      ", the coordinator uses " + parameters.ffmpeg_build,
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

  const std::filesystem::path& source = inputs.at(std::string(kOcrCropBatchSourceInput)).path;
  std::vector<nlohmann::json> records;
  std::vector<std::byte> images;
  std::size_t extracted = 0;
  for (const EvidenceCropJob& job : parameters.jobs) {
    svp::exec::throw_if_cancelled(cancellation, "ocr.crop_batch between jobs");
    const EvidenceCropJobOutcome outcome =
        run_evidence_crop_job(environment.ffmpeg_path, source, session, pp_ocr, job,
                              environment.scratch_dir / scratch_name(spec, job));
    if (!outcome.extracted) {
      records.push_back(not_extracted(job));
      continue;
    }
    ++extracted;
    nlohmann::json record = detail::append_data(images, outcome.image);
    record["extracted"] = true;
    record["ordinal"] = job.ordinal;
    // JSON cannot carry a non-finite score; the stage re-reads such a crop
    // itself.
    const bool finite = std::isfinite(outcome.roi.score);
    record["roi_decoded"] = outcome.roi.decoded && finite;
    record["roi_score"] = finite ? outcome.roi.score : 0.0;
    record["roi_text"] = outcome.roi.text;
    records.push_back(std::move(record));
  }
  svp::exec::throw_if_cancelled(cancellation, "ocr.crop_batch before output");
  return succeeded(spec, environment, records, images, extracted);
}

}  // namespace

void register_ocr_crop_batch_task(svp::exec::TaskTypeRegistry& registry,
                                  DispatchedTaskEnvironment environment,
                                  std::shared_ptr<PpOcrSessionPool> sessions) {
  if (!sessions) {
    sessions = std::make_shared<PpOcrSessionPool>();
  }
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kOcrCropBatchTaskType),
      .version = kOcrCropBatchTaskTypeVersion,
      .validate_parameters = validate_ocr_crop_batch_parameters,
      .execute = [environment = std::move(environment), sessions](
                     const TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                     const svp::exec::CancellationToken& cancellation) {
        return execute(spec, inputs, cancellation, environment, *sessions);
      }});
}

}  // namespace svp::vision::tasks
