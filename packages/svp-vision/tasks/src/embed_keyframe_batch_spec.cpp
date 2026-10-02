#include "svp/vision/tasks/embed_keyframe_batch_spec.hpp"

#include "numeric_bytes.hpp"
#include "task_results.hpp"
#include "task_spec_assembly.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::vision::tasks {

svp::exec::TaskOrderKey embed_keyframe_batch_order_key(const ItemBatch& batch) {
  return detail::item_batch_order_key(kEmbedKeyframeBatchLane, batch);
}

svp::exec::TaskSpec make_embed_keyframe_batch_task_spec(
    const EmbedKeyframeBatchTaskInputs& inputs,
    const std::vector<KeyframeEmbeddingItem>& keyframes, const ItemBatch& batch) {
  if (batch.count == 0 || batch.first >= keyframes.size() ||
      batch.count > keyframes.size() - batch.first) {
    throw std::invalid_argument("embed.keyframe_batch batch is outside the keyframes");
  }
  EmbedKeyframeBatchParameters parameters;
  parameters.model = inputs.model;
  parameters.embedding_dim = inputs.embedding_dim;
  parameters.ffmpeg_build = inputs.ffmpeg_build;
  for (std::uint64_t index = batch.first; index < batch.first + batch.count; ++index) {
    parameters.keyframes.push_back({.ordinal = index, .item = keyframes[index]});
  }
  return detail::assemble_task_spec(detail::SpecAssembly{
      .build_session_id = inputs.build_session_id,
      .depends_on = inputs.depends_on,
      .task_type = std::string(kEmbedKeyframeBatchTaskType),
      .task_type_version = kEmbedKeyframeBatchTaskTypeVersion,
      .task_id = detail::item_batch_task_id(kEmbedKeyframeBatchTaskType, batch),
      .model_refs = {inputs.model_ref},
      .source = inputs.source,
      .source_input_name = std::string(kEmbedKeyframeBatchSourceInput),
      .parameters = embed_keyframe_batch_parameters_to_json(parameters),
      .resources = svp::exec::TaskResources{
          .est_peak_rss_mb = kEmbedKeyframeBatchEstimatedPeakRssMb,
          .est_cpu_threads =
              static_cast<std::uint64_t>(std::max(1, inputs.model.threads.intra_op)),
          .est_seconds = item_batch_estimated_seconds(inputs.batch_policy, batch.count)},
  });
}

std::vector<KeyframeEmbeddingOutcome> read_embed_keyframe_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads) {
  const EmbedKeyframeBatchParameters parameters =
      embed_keyframe_batch_parameters_from_json(spec.parameters);
  const detail::RecordsAndData read = detail::read_records_and_data(
      spec, outputs, payloads, kEmbedKeyframeRecordsRole, kEmbedKeyframeVectorsRole);
  if (read.records.size() != parameters.keyframes.size()) {
    throw std::invalid_argument(spec.task_id + ": output has " +
                                std::to_string(read.records.size()) + " records for " +
                                std::to_string(parameters.keyframes.size()) + " keyframes");
  }
  std::vector<KeyframeEmbeddingOutcome> outcomes;
  outcomes.reserve(read.records.size());
  for (std::size_t index = 0; index < read.records.size(); ++index) {
    const nlohmann::json& record = read.records[index];
    const std::string where = spec.task_id + " record " + std::to_string(index);
    if (record.at("ordinal").get<std::uint64_t>() != parameters.keyframes[index].ordinal) {
      throw std::invalid_argument(where + " is not keyframe " +
                                  std::to_string(parameters.keyframes[index].ordinal));
    }
    KeyframeEmbeddingOutcome outcome;
    outcome.embedded = record.at("embedded").get<bool>();
    if (outcome.embedded) {
      outcome.vector = detail::values_from_bytes<float>(
          detail::record_data(record, read.data, where), parameters.embedding_dim, where);
    }
    outcomes.push_back(std::move(outcome));
  }
  return outcomes;
}

}  // namespace svp::vision::tasks
