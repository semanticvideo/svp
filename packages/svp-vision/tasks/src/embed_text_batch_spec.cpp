#include "svp/vision/tasks/embed_text_batch_spec.hpp"

#include "numeric_bytes.hpp"
#include "task_results.hpp"
#include "task_spec_assembly.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::vision::tasks {

svp::exec::TaskOrderKey embed_text_batch_order_key(const ItemBatch& batch) {
  return detail::item_batch_order_key(kEmbedTextBatchLane, batch);
}

svp::exec::TaskSpec make_embed_text_batch_task_spec(const EmbedTextBatchTaskInputs& inputs,
                                                    const std::vector<TextEmbeddingItem>& items,
                                                    const ItemBatch& batch) {
  if (batch.count == 0 || batch.first >= items.size() ||
      batch.count > items.size() - batch.first) {
    throw std::invalid_argument("embed.text_batch batch is outside the items");
  }
  EmbedTextBatchParameters parameters;
  parameters.model = inputs.model;
  parameters.embedding_dim = inputs.embedding_dim;
  for (std::uint64_t index = batch.first; index < batch.first + batch.count; ++index) {
    parameters.items.push_back({.ordinal = index, .item = items[index]});
  }
  return detail::assemble_task_spec(detail::SpecAssembly{
      .build_session_id = inputs.build_session_id,
      .depends_on = inputs.depends_on,
      .task_type = std::string(kEmbedTextBatchTaskType),
      .task_type_version = kEmbedTextBatchTaskTypeVersion,
      .task_id = detail::item_batch_task_id(kEmbedTextBatchTaskType, batch),
      .model_refs = {inputs.model_ref},
      .source = std::nullopt,
      .source_input_name = {},
      .parameters = embed_text_batch_parameters_to_json(parameters),
      .resources = svp::exec::TaskResources{
          .est_peak_rss_mb = kEmbedTextBatchEstimatedPeakRssMb,
          .est_cpu_threads =
              static_cast<std::uint64_t>(std::max(1, inputs.model.threads.intra_op)),
          .est_seconds = item_batch_estimated_seconds(inputs.batch_policy, batch.count)},
  });
}

std::vector<TextEmbeddingOutcome> read_embed_text_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads) {
  const EmbedTextBatchParameters parameters =
      embed_text_batch_parameters_from_json(spec.parameters);
  const detail::RecordsAndData read = detail::read_records_and_data(
      spec, outputs, payloads, kEmbedTextRecordsRole, kEmbedTextVectorsRole);
  if (read.records.size() != parameters.items.size()) {
    throw std::invalid_argument(spec.task_id + ": output has " +
                                std::to_string(read.records.size()) + " records for " +
                                std::to_string(parameters.items.size()) + " items");
  }
  std::vector<TextEmbeddingOutcome> outcomes;
  outcomes.reserve(read.records.size());
  for (std::size_t index = 0; index < read.records.size(); ++index) {
    const nlohmann::json& record = read.records[index];
    const std::string where = spec.task_id + " record " + std::to_string(index);
    if (record.at("ordinal").get<std::uint64_t>() != parameters.items[index].ordinal) {
      throw std::invalid_argument(where + " is not item " +
                                  std::to_string(parameters.items[index].ordinal));
    }
    TextEmbeddingOutcome outcome;
    if (const auto error = record.find("error"); error != record.end()) {
      outcome.error = error->get<std::string>();
      if (outcome.error.empty()) {
        throw std::invalid_argument(where + " has an empty error");
      }
    } else {
      outcome.vector = detail::values_from_bytes<float>(
          detail::record_data(record, read.data, where), parameters.embedding_dim, where);
    }
    outcomes.push_back(std::move(outcome));
  }
  return outcomes;
}

}  // namespace svp::vision::tasks
