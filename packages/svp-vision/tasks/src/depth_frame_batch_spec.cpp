#include "svp/vision/tasks/depth_frame_batch_spec.hpp"

#include "numeric_bytes.hpp"
#include "task_results.hpp"
#include "task_spec_assembly.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::vision::tasks {

svp::exec::TaskOrderKey depth_frame_batch_order_key(const ItemBatch& batch) {
  return detail::item_batch_order_key(kDepthFrameBatchLane, batch);
}

std::uint64_t depth_frame_field_bytes(const DepthFrameItem& frame) {
  return static_cast<std::uint64_t>(frame.width) * static_cast<std::uint64_t>(frame.height) *
         sizeof(std::uint16_t);
}

svp::exec::TaskSpec make_depth_frame_batch_task_spec(const DepthFrameBatchTaskInputs& inputs,
                                                     const std::vector<DepthFrameItem>& frames,
                                                     const ItemBatch& batch) {
  if (batch.count == 0 || batch.first >= frames.size() ||
      batch.count > frames.size() - batch.first) {
    throw std::invalid_argument("depth.frame_batch batch is outside the frames");
  }
  DepthFrameBatchParameters parameters;
  parameters.model = inputs.model;
  parameters.ffmpeg_build = inputs.ffmpeg_build;
  const auto first = frames.begin() + static_cast<std::ptrdiff_t>(batch.first);
  parameters.frames.assign(first, first + static_cast<std::ptrdiff_t>(batch.count));
  return detail::assemble_task_spec(detail::SpecAssembly{
      .build_session_id = inputs.build_session_id,
      .depends_on = inputs.depends_on,
      .task_type = std::string(kDepthFrameBatchTaskType),
      .task_type_version = kDepthFrameBatchTaskTypeVersion,
      .task_id = detail::item_batch_task_id(kDepthFrameBatchTaskType, batch),
      .model_refs = {inputs.model_ref},
      .source = inputs.source,
      .source_input_name = std::string(kDepthFrameBatchSourceInput),
      .parameters = depth_frame_batch_parameters_to_json(parameters),
      .resources = svp::exec::TaskResources{
          .est_peak_rss_mb = kDepthFrameBatchEstimatedPeakRssMb,
          .est_cpu_threads =
              static_cast<std::uint64_t>(std::max(1, inputs.model.threads.intra_op)),
          .est_seconds = item_batch_estimated_seconds(inputs.batch_policy, batch.count)},
  });
}

std::vector<DepthFrameOutcome> read_depth_frame_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads) {
  const DepthFrameBatchParameters parameters =
      depth_frame_batch_parameters_from_json(spec.parameters);
  const detail::RecordsAndData read = detail::read_records_and_data(
      spec, outputs, payloads, kDepthFrameRecordsRole, kDepthFrameFieldsRole);
  if (read.records.size() != parameters.frames.size()) {
    throw std::invalid_argument(spec.task_id + ": output has " +
                                std::to_string(read.records.size()) + " records for " +
                                std::to_string(parameters.frames.size()) + " frames");
  }
  std::vector<DepthFrameOutcome> outcomes;
  outcomes.reserve(read.records.size());
  for (std::size_t index = 0; index < read.records.size(); ++index) {
    const nlohmann::json& record = read.records[index];
    const DepthFrameItem& frame = parameters.frames[index];
    const std::string where = spec.task_id + " record " + std::to_string(index);
    if (record.at("ordinal").get<std::uint64_t>() != frame.ordinal) {
      throw std::invalid_argument(where + " is not frame " + std::to_string(frame.ordinal));
    }
    DepthFrameOutcome outcome;
    const std::string status = record.at("status").get<std::string>();
    if (status == "ok") {
      outcome.status = DepthFrameStatus::ok;
      outcome.depth = detail::values_from_bytes<std::uint16_t>(
          detail::record_data(record, read.data, where),
          static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height), where);
    } else if (status == "failed") {
      // Any failure: the stage runs the frame itself, so only the status
      // matters here.
      outcome.status = DepthFrameStatus::inference_failed;
    } else {
      throw std::invalid_argument(where + " has unknown status `" + status + "`");
    }
    outcomes.push_back(std::move(outcome));
  }
  return outcomes;
}

}  // namespace svp::vision::tasks
