#include "depth_frame_batch_execution.hpp"

#include "numeric_bytes.hpp"
#include "onnx_task_common.hpp"
#include "task_results.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/depth_frame_work.hpp"
#include "svp/vision/tasks/depth_frame_batch_parameters.hpp"

namespace svp::vision::tasks::detail {
namespace {

svp::exec::TaskResult succeeded(const svp::exec::TaskSpec& spec,
                                const DispatchedTaskEnvironment& environment,
                                const std::vector<nlohmann::json>& records,
                                const std::vector<std::byte>& fields, std::size_t ok,
                                std::size_t pixel_mismatches) {
  return succeeded_result(spec, environment.write_output, records, fields,
                          kDepthFrameRecordsRole, kDepthFrameFieldsRole,
                          {{"frames", records.size()},
                           {"ok", ok},
                           {"pixel_mismatches", pixel_mismatches}});
}

nlohmann::json failed_frame(const DepthFrameItem& frame) {
  return {{"ordinal", frame.ordinal}, {"status", "failed"}};
}

}  // namespace

svp::exec::TaskResult execute_depth_frame_batch(const svp::exec::TaskSpec& spec,
                                                const svp::exec::ResolvedInputs& inputs,
                                                const svp::exec::CancellationToken& cancellation,
                                                const DispatchedTaskEnvironment& environment,
                                                OnnxModelPool& models) {
  svp::exec::throw_if_cancelled(cancellation, "depth.frame_batch start");
  const DepthFrameBatchParameters parameters =
      depth_frame_batch_parameters_from_json(spec.parameters);
  if (spec.inputs.size() != 1 ||
      !spec.inputs.contains(std::string(kDepthFrameBatchSourceInput))) {
    return failed_result(spec, kDepthFrameBatchTaskType, "invalid_inputs",
                         "inputs must be exactly `" + std::string(kDepthFrameBatchSourceInput) +
                             "`",
                         false);
  }
  const CouldNotStart could_not_start = [&](std::string code, std::string reason) {
    if (!environment.record_start_failures) {
      return failed_result(spec, kDepthFrameBatchTaskType, std::move(code), std::move(reason),
                           true);
    }
    // The stage runs every frame itself (svp/vision/dispatched_work.hpp).
    std::vector<nlohmann::json> records;
    for (const DepthFrameItem& frame : parameters.frames) {
      records.push_back(failed_frame(frame));
    }
    return succeeded(spec, environment, records, {}, 0, 0);
  };
  if (auto ended = check_ffmpeg_build(spec, kDepthFrameBatchTaskType, parameters.ffmpeg_build,
                                      environment, could_not_start)) {
    return std::move(*ended);
  }
  OnnxTaskStart start = start_onnx_task(spec, kDepthFrameBatchTaskType, parameters.model, false,
                                        environment, models, could_not_start);
  if (start.result) {
    return std::move(*start.result);
  }
  const LoadedOnnxModel& model = start.model->model();
  const std::filesystem::path& source = inputs.at(std::string(kDepthFrameBatchSourceInput)).path;

  std::vector<nlohmann::json> records;
  std::vector<std::byte> fields;
  std::size_t ok = 0;
  std::size_t pixel_mismatches = 0;
  for (const DepthFrameItem& item : parameters.frames) {
    svp::exec::throw_if_cancelled(cancellation, "depth.frame_batch between frames");
    std::string decode_error;
    std::vector<Srgb8Pixel> pixels = decode_rgb_frame_at(environment.ffmpeg_path, source,
                                                         item.timestamp_us, item.width,
                                                         item.height, decode_error);
    if (pixels.empty() || canonical_frame_pixels_blake3(pixels) != item.pixels_blake3) {
      ++pixel_mismatches;
      records.push_back(failed_frame(item));
      continue;
    }
    const ColorRasterFrame frame{item.frame_id, item.timestamp_us, item.width, item.height,
                                 false, std::move(pixels)};
    const DepthFrameOutcome outcome = infer_depth_frame_outcome(model.session, frame);
    if (outcome.status != DepthFrameStatus::ok) {
      records.push_back(failed_frame(item));
      continue;
    }
    ++ok;
    nlohmann::json record = append_data(fields, values_to_bytes(outcome.depth));
    record["ordinal"] = item.ordinal;
    record["status"] = "ok";
    records.push_back(std::move(record));
  }
  svp::exec::throw_if_cancelled(cancellation, "depth.frame_batch before output");
  return succeeded(spec, environment, records, fields, ok, pixel_mismatches);
}

}  // namespace svp::vision::tasks::detail
