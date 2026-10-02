#include "calibration/dispatched_capacity_workloads.hpp"

#include "svp/media/canonical_raster.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/tasks/depth_frame_batch_spec.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_spec.hpp"
#include "svp/vision/tasks/embed_text_batch_spec.hpp"
#include "svp/vision/tasks/ocr_calibration_clip.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_crop_batch_spec.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"

#include <stdexcept>

namespace svp::builder::calibration {
namespace {

namespace tasks = svp::vision::tasks;

constexpr const char* kCalibrationSession = "bs_dispatch_calibration";
// The OCR stage's crop policy, under which every observation gets its crop.
constexpr const char* kOneCropPerObservation = "one_per_observation";

// Calibration specs carry the minimum estimate: their leases come from the
// lease policy's floors (30 s, renewed by heartbeats; 10 min hard deadline),
// which bound one calibration batch on any Mac.
constexpr std::uint64_t kCalibrationEstimatedSeconds = 1;

// Spec builders need a batch policy only for est_seconds, which calibration
// specs override (kCalibrationEstimatedSeconds).
tasks::ItemBatchPolicy unmeasured_policy() {
  tasks::ItemBatchPolicy policy;
  policy.estimated_seconds_per_item = policy.target_task_seconds;
  return policy;
}

svp::exec::TaskSpec with_minimum_estimate(svp::exec::TaskSpec spec) {
  spec.resources.est_seconds = kCalibrationEstimatedSeconds;
  return spec;
}

template <typename Make, typename Items>
CapacityWorkload workload(std::string_view task_type, const Items& items, Make make) {
  if (items.empty()) {
    throw std::runtime_error(std::string(task_type) + " calibration has no items");
  }
  return CapacityWorkload{
      .name = std::string(task_type),
      .batch = with_minimum_estimate(make(tasks::ItemBatch{.first = 0, .count = items.size()})),
      .warm_up = with_minimum_estimate(make(tasks::ItemBatch{.first = 0, .count = 1})),
      .items_per_batch = items.size()};
}

const DistributedOnnxWork& onnx_work(const std::optional<DistributedOnnxWork>& work,
                                     std::string_view task_type) {
  if (!work) {
    throw std::runtime_error("this build does not dispatch " + std::string(task_type));
  }
  return *work;
}

tasks::OnnxModelParameters onnx_parameters(const DistributedOnnxWork& work) {
  return tasks::OnnxModelParameters{.model_id = work.model.model_id,
                                    .execution_provider = work.model.execution_provider,
                                    .threads = work.model.threads};
}

svp::media::CanonicalAnalysisRaster clip_raster() {
  return svp::media::compute_canonical_analysis_raster(svp::media::SourceDisplayGeometry{
      .stored_width = tasks::kOcrCalibrationFrameWidth,
      .stored_height = tasks::kOcrCalibrationFrameHeight});
}

CapacityWorkload crop_workload(const DispatchedCalibrationSetup& setup,
                               const svp::exec::ArtifactRef& clip) {
  const std::vector<std::int64_t> timestamps = tasks::ocr_calibration_timestamps_us();
  svp::vision::EvidenceCropOptions options;
  options.ocr_frame_width = tasks::kOcrCalibrationFrameWidth;
  options.ocr_frame_height = tasks::kOcrCalibrationFrameHeight;
  options.source_frame_width = tasks::kOcrCalibrationFrameWidth;
  options.source_frame_height = tasks::kOcrCalibrationFrameHeight;
  options.crop_coverage_policy = kOneCropPerObservation;
  options.crop_image_format = svp::vision::kEvidenceCropImageFormat;
  options.jpeg_quality = svp::vision::kEvidenceCropJpegQuality;
  std::vector<svp::vision::CropGenerationInput> inputs;
  for (const tasks::OcrCalibrationTextLine& line : tasks::ocr_calibration_text_lines()) {
    if (line.frame != kCropCalibrationFrame) {
      continue;
    }
    svp::vision::CropGenerationInput input;
    input.source_timestamp_us = timestamps.at(line.frame);
    input.bbox_left = line.left;
    input.bbox_top = line.top;
    input.bbox_right = line.right;
    input.bbox_bottom = line.bottom;
    input.frame_width = tasks::kOcrCalibrationFrameWidth;
    input.frame_height = tasks::kOcrCalibrationFrameHeight;
    inputs.push_back(std::move(input));
  }
  const std::vector<svp::vision::EvidenceCropJob> jobs =
      svp::vision::plan_evidence_crop_jobs(options, inputs);
  const tasks::OcrCropBatchTaskInputs task_inputs{.build_session_id = kCalibrationSession,
                                                  .depends_on = {},
                                                  .source = clip,
                                                  .model_refs = setup.pp_ocr_model_refs,
                                                  .pp_ocr = setup.pp_ocr,
                                                  .ffmpeg_build = setup.ffmpeg_build,
                                                  .batch_policy = unmeasured_policy()};
  return workload(tasks::kOcrCropBatchTaskType, jobs, [&](const tasks::ItemBatch& batch) {
    return tasks::make_ocr_crop_batch_task_spec(task_inputs, jobs, batch);
  });
}

CapacityWorkload text_workload(const DispatchedCalibrationSetup& setup) {
  const DistributedOnnxWork& work =
      onnx_work(setup.vision.text_embeddings, tasks::kEmbedTextBatchTaskType);
  std::vector<svp::vision::TextEmbeddingItem> items;
  for (const tasks::OcrCalibrationTextLine& line : tasks::ocr_calibration_text_lines()) {
    items.push_back({.id = "calibration_line_" + std::to_string(items.size()), .text = line.text});
  }
  const tasks::EmbedTextBatchTaskInputs task_inputs{.build_session_id = kCalibrationSession,
                                                    .depends_on = {},
                                                    .model_ref = work.model_ref,
                                                    .model = onnx_parameters(work),
                                                    .embedding_dim = setup.vision.embedding_dim,
                                                    .batch_policy = unmeasured_policy()};
  return workload(tasks::kEmbedTextBatchTaskType, items, [&](const tasks::ItemBatch& batch) {
    return tasks::make_embed_text_batch_task_spec(task_inputs, items, batch);
  });
}

CapacityWorkload keyframe_workload(const DispatchedCalibrationSetup& setup,
                                   const svp::exec::ArtifactRef& clip) {
  const DistributedOnnxWork& work =
      onnx_work(setup.vision.keyframe_embeddings, tasks::kEmbedKeyframeBatchTaskType);
  const svp::media::CanonicalAnalysisRaster raster = clip_raster();
  std::vector<svp::vision::KeyframeEmbeddingItem> items;
  for (std::size_t repeat = 0; repeat < kKeyframeCalibrationRepeats; ++repeat) {
    for (const std::int64_t timestamp : tasks::ocr_calibration_timestamps_us()) {
      items.push_back({.shot_id = "calibration_shot_" + std::to_string(items.size()),
                       .pts_us = timestamp,
                       .width = raster.width,
                       .height = raster.height});
    }
  }
  const tasks::EmbedKeyframeBatchTaskInputs task_inputs{
      .build_session_id = kCalibrationSession,
      .depends_on = {},
      .source = clip,
      .model_ref = work.model_ref,
      .model = onnx_parameters(work),
      .embedding_dim = setup.vision.embedding_dim,
      .ffmpeg_build = setup.ffmpeg_build,
      .batch_policy = unmeasured_policy()};
  return workload(tasks::kEmbedKeyframeBatchTaskType, items, [&](const tasks::ItemBatch& batch) {
    return tasks::make_embed_keyframe_batch_task_spec(task_inputs, items, batch);
  });
}

CapacityWorkload depth_workload(const DispatchedCalibrationSetup& setup,
                                const svp::exec::ArtifactRef& clip,
                                const std::filesystem::path& clip_file,
                                const std::filesystem::path& ffmpeg) {
  const DistributedOnnxWork& work = onnx_work(setup.vision.depth, tasks::kDepthFrameBatchTaskType);
  const svp::media::CanonicalAnalysisRaster raster = clip_raster();
  std::vector<svp::vision::ColorRasterFrame> decoded;
  for (const std::int64_t timestamp : tasks::ocr_calibration_timestamps_us()) {
    std::string error;
    std::vector<svp::vision::Srgb8Pixel> pixels = svp::vision::decode_rgb_frame_at(
        ffmpeg, clip_file, timestamp, raster.width, raster.height, error);
    if (pixels.empty()) {
      throw std::runtime_error("cannot decode the calibration clip for depth: " + error);
    }
    decoded.push_back({"calibration_frame_" + std::to_string(decoded.size()), timestamp,
                       raster.width, raster.height, false, std::move(pixels)});
  }
  std::vector<tasks::DepthFrameItem> items;
  for (std::size_t repeat = 0; repeat < kDepthCalibrationRepeats; ++repeat) {
    for (const svp::vision::ColorRasterFrame& frame : decoded) {
      items.push_back(tasks::depth_frame_item(frame, items.size()));
    }
  }
  const tasks::DepthFrameBatchTaskInputs task_inputs{.build_session_id = kCalibrationSession,
                                                     .depends_on = {},
                                                     .source = clip,
                                                     .model_ref = work.model_ref,
                                                     .model = onnx_parameters(work),
                                                     .ffmpeg_build = setup.ffmpeg_build,
                                                     .batch_policy = unmeasured_policy()};
  return workload(tasks::kDepthFrameBatchTaskType, items, [&](const tasks::ItemBatch& batch) {
    return tasks::make_depth_frame_batch_task_spec(task_inputs, items, batch);
  });
}

}  // namespace

std::vector<std::string> dispatched_task_types(const DistributedVisionWork& vision) {
  std::vector<std::string> types;
  if (vision.evidence_crops) types.emplace_back(tasks::kOcrCropBatchTaskType);
  if (vision.text_embeddings) types.emplace_back(tasks::kEmbedTextBatchTaskType);
  if (vision.keyframe_embeddings) types.emplace_back(tasks::kEmbedKeyframeBatchTaskType);
  if (vision.depth) types.emplace_back(tasks::kDepthFrameBatchTaskType);
  return types;
}

std::uint64_t dispatched_task_peak_rss_mb(std::string_view task_type) {
  if (task_type == tasks::kOcrCropBatchTaskType) return tasks::kOcrCropBatchEstimatedPeakRssMb;
  if (task_type == tasks::kEmbedTextBatchTaskType) return tasks::kEmbedTextBatchEstimatedPeakRssMb;
  if (task_type == tasks::kEmbedKeyframeBatchTaskType) {
    return tasks::kEmbedKeyframeBatchEstimatedPeakRssMb;
  }
  if (task_type == tasks::kDepthFrameBatchTaskType) return tasks::kDepthFrameBatchEstimatedPeakRssMb;
  throw std::invalid_argument("not a dispatched task type: " + std::string(task_type));
}

CapacityWorkload dispatched_capacity_workload(std::string_view task_type,
                                              const DispatchedCalibrationSetup& setup,
                                              const svp::exec::ArtifactRef& clip,
                                              const std::filesystem::path& clip_file,
                                              const std::filesystem::path& ffmpeg) {
  if (task_type == tasks::kOcrCropBatchTaskType) return crop_workload(setup, clip);
  if (task_type == tasks::kEmbedTextBatchTaskType) return text_workload(setup);
  if (task_type == tasks::kEmbedKeyframeBatchTaskType) return keyframe_workload(setup, clip);
  if (task_type == tasks::kDepthFrameBatchTaskType) {
    return depth_workload(setup, clip, clip_file, ffmpeg);
  }
  throw std::runtime_error("not a dispatched task type: " + std::string(task_type));
}

}  // namespace svp::builder::calibration
