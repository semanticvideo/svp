#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cache_key.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/models/manifest.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace svp::vision::tasks {
namespace {

svp::exec::TaskModelRef model_ref(const PpOcrOptions& pp_ocr, const std::string& model_id) {
  const std::filesystem::path manifest_path =
      pp_ocr.model_cache_root / model_id / pp_ocr.manifest_filename;
  const svp::models::ModelBundleManifest manifest = [&] {
    try {
      return svp::models::load_model_bundle_manifest(manifest_path);
    } catch (const std::exception& error) {
      throw std::runtime_error("ocr.frame_batch model ref for " + model_id + ": " +
                               error.what());
    }
  }();
  const auto bundle_blake3 =
      svp::exec::parse_blake3_hex(manifest.bundle_blake3.hex_value());
  if (manifest.model_id != model_id || !bundle_blake3) {
    throw std::runtime_error("ocr.frame_batch model ref for " + model_id +
                             ": manifest " + manifest_path.string() +
                             " does not identify that model by BLAKE3");
  }
  return svp::exec::TaskModelRef{.model_id = manifest.model_id,
                                 .model_bundle_id = manifest.model_bundle_id,
                                 .bundle_blake3 = *bundle_blake3};
}

std::string padded_ordinal(std::uint64_t ordinal) {
  std::ostringstream oss;
  oss << std::setw(6) << std::setfill('0') << ordinal;
  return oss.str();
}

// Admission estimate: detection and recognition run one after the other, and
// recognition workers share the recognizer session's pools, so the busiest
// phase uses about the largest of these.
std::uint64_t estimated_cpu_threads(const PpOcrOptions& pp_ocr) {
  return static_cast<std::uint64_t>(std::max({1, pp_ocr.det_threads.intra_op,
                                              pp_ocr.rec_threads.intra_op,
                                              pp_ocr.recognition_parallel_workers}));
}

std::uint64_t estimated_seconds(const OcrBatchPolicy& policy, std::uint64_t samples) {
  validate_ocr_batch_policy(policy);
  const double seconds =
      std::ceil(static_cast<double>(samples) * policy.estimated_seconds_per_sample);
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(seconds));
}

}  // namespace

std::vector<svp::exec::TaskModelRef> ocr_frame_batch_model_refs(const PpOcrOptions& pp_ocr) {
  std::vector<svp::exec::TaskModelRef> refs = {
      model_ref(pp_ocr, pp_ocr.detector_model_id),
      model_ref(pp_ocr, pp_ocr.recognizer_model_id),
  };
  std::sort(refs.begin(), refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  return refs;
}

std::string ocr_frame_batch_task_id(const OcrSampleBatch& batch) {
  const std::uint64_t last = batch.first_ordinal + batch.count - 1;
  return "task.ocr.frame_batch.samples_" + padded_ordinal(batch.first_ordinal) + "_" +
         padded_ordinal(last);
}

svp::exec::TaskOrderKey ocr_frame_batch_order_key(const OcrSampleBatch& batch) {
  return svp::exec::TaskOrderKey{.lane = std::string(kOcrFrameBatchLane),
                                 .ordinals = {batch.first_ordinal}};
}

svp::exec::TaskSpec make_ocr_frame_batch_task_spec(const OcrFrameBatchTaskInputs& inputs,
                                                   const OcrSamplePlan& plan,
                                                   const OcrSampleBatch& batch) {
  if (batch.count == 0 || batch.first_ordinal >= plan.samples.size() ||
      batch.count > plan.samples.size() - batch.first_ordinal) {
    throw std::invalid_argument("ocr.frame_batch batch is outside the sample plan");
  }
  OcrFrameBatchParameters parameters;
  const auto first =
      plan.samples.begin() + static_cast<std::ptrdiff_t>(batch.first_ordinal);
  parameters.samples.assign(first, first + static_cast<std::ptrdiff_t>(batch.count));
  parameters.frame_width = plan.frame_width;
  parameters.frame_height = plan.frame_height;
  parameters.pp_ocr = inputs.pp_ocr;
  parameters.ffmpeg_build = inputs.ffmpeg_build;

  svp::exec::TaskSpec spec;
  spec.build_session_id = inputs.build_session_id;
  spec.task_id = ocr_frame_batch_task_id(batch);
  spec.task_type = std::string(kOcrFrameBatchTaskType);
  spec.task_type_version = kOcrFrameBatchTaskTypeVersion;
  spec.depends_on = inputs.depends_on;
  std::sort(spec.depends_on.begin(), spec.depends_on.end());
  spec.model_refs = inputs.model_refs;
  std::sort(spec.model_refs.begin(), spec.model_refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  spec.inputs.emplace(std::string(kOcrFrameBatchSourceInput), inputs.source);
  spec.parameters = ocr_frame_batch_parameters_to_json(parameters);
  spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);

  nlohmann::json bundle_ids = nlohmann::json::array();
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    bundle_ids.push_back(ref.model_bundle_id);
  }
  spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array({
      spec.task_type,
      spec.task_type_version,
      svp::exec::blake3_hex(inputs.source.blake3),
      std::move(bundle_ids),
      svp::exec::blake3_hex(spec.parameters_blake3),
  }));
  spec.resources = svp::exec::TaskResources{
      .est_peak_rss_mb = kOcrFrameBatchEstimatedPeakRssMb,
      .est_cpu_threads = estimated_cpu_threads(inputs.pp_ocr),
      .est_seconds = estimated_seconds(inputs.batch_policy, batch.count),
  };
  svp::exec::validate_task_spec(spec);
  return spec;
}

std::vector<OcrSampleDetections> read_ocr_frame_batch_output(const svp::exec::TaskSpec& spec,
                                                             std::string_view payload) {
  const OcrFrameBatchParameters parameters =
      ocr_frame_batch_parameters_from_json(spec.parameters);
  std::vector<OcrSampleDetections> records = decode_ocr_sample_detections_jsonl(payload);
  if (records.size() != parameters.samples.size()) {
    throw std::invalid_argument(spec.task_id + ": output has " +
                                std::to_string(records.size()) + " records for " +
                                std::to_string(parameters.samples.size()) + " samples");
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const OcrSample& expected = parameters.samples[index];
    if (records[index].sample_ordinal != expected.ordinal ||
        records[index].timestamp_us != expected.timestamp_us) {
      throw std::invalid_argument(spec.task_id + ": record " + std::to_string(index) +
                                  " is not sample " + std::to_string(expected.ordinal) +
                                  " at " + std::to_string(expected.timestamp_us) + " us");
    }
  }
  return records;
}

}  // namespace svp::vision::tasks
