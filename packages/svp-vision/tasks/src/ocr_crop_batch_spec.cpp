#include "svp/vision/tasks/ocr_crop_batch_spec.hpp"

#include "task_results.hpp"
#include "task_spec_assembly.hpp"

#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::vision::tasks {
namespace {

// Admission estimate: the ROI re-read runs one recognizer call per crop
// beside one ffmpeg process (extraction, then decoding back).
std::uint64_t estimated_cpu_threads(const PpOcrOptions& pp_ocr) {
  return static_cast<std::uint64_t>(std::max(1, pp_ocr.rec_threads.intra_op));
}

}  // namespace

svp::exec::TaskOrderKey ocr_crop_batch_order_key(const ItemBatch& batch) {
  return detail::item_batch_order_key(kOcrCropBatchLane, batch);
}

std::uint64_t ocr_crop_job_parameter_bytes(const EvidenceCropJob& job) {
  return nlohmann::json{{"height", job.height},
                        {"image_format", job.image_format},
                        {"jpeg_quality", job.jpeg_quality},
                        {"left", job.left},
                        {"ordinal", job.ordinal},
                        {"seek_us", job.seek_us},
                        {"top", job.top},
                        {"width", job.width}}
      .dump()
      .size();
}

svp::exec::TaskSpec make_ocr_crop_batch_task_spec(const OcrCropBatchTaskInputs& inputs,
                                                  const std::vector<EvidenceCropJob>& jobs,
                                                  const ItemBatch& batch) {
  if (batch.count == 0 || batch.first >= jobs.size() ||
      batch.count > jobs.size() - batch.first) {
    throw std::invalid_argument("ocr.crop_batch batch is outside the crop jobs");
  }
  OcrCropBatchParameters parameters;
  const auto first = jobs.begin() + static_cast<std::ptrdiff_t>(batch.first);
  parameters.jobs.assign(first, first + static_cast<std::ptrdiff_t>(batch.count));
  parameters.ffmpeg_build = inputs.ffmpeg_build;
  parameters.pp_ocr = inputs.pp_ocr;
  return detail::assemble_task_spec(detail::SpecAssembly{
      .build_session_id = inputs.build_session_id,
      .depends_on = inputs.depends_on,
      .task_type = std::string(kOcrCropBatchTaskType),
      .task_type_version = kOcrCropBatchTaskTypeVersion,
      .task_id = detail::item_batch_task_id(kOcrCropBatchTaskType, batch),
      .model_refs = inputs.model_refs,
      .source = inputs.source,
      .source_input_name = std::string(kOcrCropBatchSourceInput),
      .parameters = ocr_crop_batch_parameters_to_json(parameters),
      .resources = svp::exec::TaskResources{
          .est_peak_rss_mb = kOcrCropBatchEstimatedPeakRssMb,
          .est_cpu_threads = estimated_cpu_threads(inputs.pp_ocr),
          .est_seconds = item_batch_estimated_seconds(inputs.batch_policy, batch.count)},
  });
}

std::vector<EvidenceCropJobOutcome> read_ocr_crop_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads) {
  const OcrCropBatchParameters parameters = ocr_crop_batch_parameters_from_json(spec.parameters);
  const detail::RecordsAndData read = detail::read_records_and_data(
      spec, outputs, payloads, kOcrCropRecordsRole, kOcrCropImagesRole);
  if (read.records.size() != parameters.jobs.size()) {
    throw std::invalid_argument(spec.task_id + ": output has " +
                                std::to_string(read.records.size()) + " records for " +
                                std::to_string(parameters.jobs.size()) + " jobs");
  }
  std::vector<EvidenceCropJobOutcome> outcomes;
  outcomes.reserve(read.records.size());
  for (std::size_t index = 0; index < read.records.size(); ++index) {
    const nlohmann::json& record = read.records[index];
    const std::string where = spec.task_id + " record " + std::to_string(index);
    EvidenceCropJobOutcome outcome;
    outcome.ordinal = record.at("ordinal").get<std::uint64_t>();
    if (outcome.ordinal != parameters.jobs[index].ordinal) {
      throw std::invalid_argument(where + " is not job " +
                                  std::to_string(parameters.jobs[index].ordinal));
    }
    outcome.extracted = record.at("extracted").get<bool>();
    if (outcome.extracted) {
      const std::span<const std::byte> image = detail::record_data(record, read.data, where);
      outcome.image.assign(image.begin(), image.end());
      outcome.roi.decoded = record.at("roi_decoded").get<bool>();
      outcome.roi.text = record.at("roi_text").get<std::string>();
      outcome.roi.score = record.at("roi_score").get<double>();
    }
    outcomes.push_back(std::move(outcome));
  }
  return outcomes;
}

}  // namespace svp::vision::tasks
