#include "calibration/ocr_capacity_calibration.hpp"

#include "calibration/capacity_sweep.hpp"

#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/tasks/ocr_calibration_clip.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace svp::builder::calibration {
namespace {

constexpr std::string_view kOcrCalibrationName = "ocr";

svp::vision::OcrSamplePlan clip_plan(const std::vector<std::int64_t>& timestamps_us) {
  svp::vision::OcrTemporalSamplingResult sampling;
  sampling.timestamps_us = timestamps_us;
  sampling.sample_count = static_cast<int>(timestamps_us.size());
  return svp::vision::make_ocr_sample_plan(
      std::move(sampling), svp::vision::tasks::kOcrCalibrationFrameWidth,
      svp::vision::tasks::kOcrCalibrationFrameHeight);
}

CapacityWorkload ocr_workload(const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
                              const std::vector<std::int64_t>& timestamps_us) {
  return CapacityWorkload{
      .name = std::string(kOcrCalibrationName),
      .batch = ocr_calibration_spec(inputs, timestamps_us),
      .warm_up = svp::vision::tasks::make_ocr_frame_batch_task_spec(
          inputs, clip_plan(timestamps_us),
          svp::vision::OcrSampleBatch{.first_ordinal = 0, .count = 1}),
      .items_per_batch = timestamps_us.size()};
}

OcrCalibration from_capacity(const CapacityCalibration& capacity) {
  OcrCalibration calibration;
  calibration.slots = capacity.slots;
  calibration.seconds_per_frame = capacity.seconds_per_item;
  calibration.stopped_because = capacity.stopped_because;
  for (const CapacityStep& step : capacity.sweep) {
    calibration.sweep.push_back(OcrCalibrationStep{.slots = step.slots,
                                                   .frames = step.items,
                                                   .wall_ms = step.wall_ms,
                                                   .frames_per_second = step.items_per_second});
  }
  return calibration;
}

}  // namespace

nlohmann::json ocr_calibration_to_json(const OcrCalibration& calibration) {
  nlohmann::json sweep = nlohmann::json::array();
  for (const OcrCalibrationStep& step : calibration.sweep) {
    sweep.push_back({{"frames", step.frames},
                     {"frames_per_second", step.frames_per_second},
                     {"slots", step.slots},
                     {"wall_ms", step.wall_ms}});
  }
  return {{"seconds_per_frame", calibration.seconds_per_frame},
          {"slots", calibration.slots},
          {"stopped_because", calibration.stopped_because},
          {"sweep", std::move(sweep)}};
}

OcrCalibration ocr_calibration_from_json(const nlohmann::json& value) {
  OcrCalibration calibration;
  calibration.slots = value.at("slots").get<std::size_t>();
  calibration.seconds_per_frame = value.at("seconds_per_frame").get<double>();
  calibration.stopped_because = value.at("stopped_because").get<std::string>();
  for (const nlohmann::json& step : value.at("sweep")) {
    calibration.sweep.push_back(OcrCalibrationStep{
        .slots = step.at("slots").get<std::size_t>(),
        .frames = step.at("frames").get<std::uint64_t>(),
        .wall_ms = step.at("wall_ms").get<std::int64_t>(),
        .frames_per_second = step.at("frames_per_second").get<double>()});
  }
  if (calibration.slots == 0 || !(calibration.seconds_per_frame > 0.0)) {
    throw std::invalid_argument("calibration needs slots >= 1 and a positive frame time");
  }
  return calibration;
}

std::size_t ocr_calibration_max_slots(std::uint64_t available_bytes, std::uint64_t reserve_bytes,
                                      std::uint32_t logical_cpus) {
  return capacity_max_slots(available_bytes, reserve_bytes, logical_cpus,
                            svp::vision::tasks::kOcrFrameBatchEstimatedPeakRssMb);
}

svp::exec::TaskSpec ocr_calibration_spec(const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
                                         const std::vector<std::int64_t>& timestamps_us) {
  const svp::vision::OcrSamplePlan plan = clip_plan(timestamps_us);
  return svp::vision::tasks::make_ocr_frame_batch_task_spec(
      inputs, plan,
      svp::vision::OcrSampleBatch{.first_ordinal = 0, .count = plan.samples.size()});
}

OcrCalibrationGraph ocr_calibration_graph(
    const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
    const std::vector<std::int64_t>& timestamps_us, std::size_t max_slots) {
  CapacityCalibrationGraph graph =
      capacity_calibration_graph(ocr_workload(inputs, timestamps_us), max_slots);
  return OcrCalibrationGraph{.nodes = std::move(graph.nodes),
                             .timed_step = std::move(graph.timed_step)};
}

OcrCalibration calibrate_ocr_capacity(svp::exec::Executor& executor, std::size_t max_slots,
                                      const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
                                      const std::vector<std::int64_t>& timestamps_us,
                                      const svp::exec::CancellationToken& cancellation) {
  return from_capacity(calibrate_capacity(executor, max_slots, ocr_workload(inputs, timestamps_us),
                                          cancellation));
}

}  // namespace svp::builder::calibration
