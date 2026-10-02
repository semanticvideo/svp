#include "embed_text_batch_execution.hpp"

#include "numeric_bytes.hpp"
#include "onnx_task_common.hpp"
#include "task_results.hpp"

#include "svp/vision/tasks/embed_text_batch_parameters.hpp"
#include "svp/vision/text_embedding_work.hpp"

namespace svp::vision::tasks::detail {
namespace {

svp::exec::TaskResult succeeded(const svp::exec::TaskSpec& spec,
                                const DispatchedTaskEnvironment& environment,
                                const std::vector<nlohmann::json>& records,
                                const std::vector<std::byte>& vectors, std::size_t embedded) {
  return succeeded_result(spec, environment.write_output, records, vectors,
                          kEmbedTextRecordsRole, kEmbedTextVectorsRole,
                          {{"embedded", embedded}, {"items", records.size()}});
}

}  // namespace

svp::exec::TaskResult execute_embed_text_batch(const svp::exec::TaskSpec& spec,
                                                const svp::exec::ResolvedInputs& /*inputs*/,
                                                const svp::exec::CancellationToken& cancellation,
                                                const DispatchedTaskEnvironment& environment,
                                                OnnxModelPool& models) {
  svp::exec::throw_if_cancelled(cancellation, "embed.text_batch start");
  const EmbedTextBatchParameters parameters =
      embed_text_batch_parameters_from_json(spec.parameters);
  if (!spec.inputs.empty()) {
    return failed_result(spec, kEmbedTextBatchTaskType, "invalid_inputs",
                         "embed.text_batch takes no inputs", false);
  }
  const CouldNotStart could_not_start = [&](std::string code, std::string reason) {
    if (!environment.record_start_failures) {
      return failed_result(spec, kEmbedTextBatchTaskType, std::move(code), std::move(reason),
                           true);
    }
    // The stage embeds every item itself (svp/vision/dispatched_work.hpp).
    std::vector<nlohmann::json> records;
    for (const OrderedTextEmbeddingItem& item : parameters.items) {
      records.push_back({{"error", "not started here: " + reason}, {"ordinal", item.ordinal}});
    }
    return succeeded(spec, environment, records, {}, 0);
  };
  OnnxTaskStart start = start_onnx_task(spec, kEmbedTextBatchTaskType, parameters.model, true,
                                        environment, models, could_not_start);
  if (start.result) {
    return std::move(*start.result);
  }
  const LoadedOnnxModel& model = start.model->model();

  std::vector<nlohmann::json> records;
  std::vector<std::byte> vectors;
  std::size_t embedded = 0;
  for (const OrderedTextEmbeddingItem& item : parameters.items) {
    svp::exec::throw_if_cancelled(cancellation, "embed.text_batch between items");
    const TextEmbeddingOutcome outcome =
        embed_text_item(model.session, model.tokenizer, item.item, parameters.embedding_dim);
    if (!outcome.error.empty()) {
      records.push_back({{"error", outcome.error}, {"ordinal", item.ordinal}});
      continue;
    }
    ++embedded;
    nlohmann::json record = append_data(vectors, values_to_bytes(outcome.vector));
    record["ordinal"] = item.ordinal;
    records.push_back(std::move(record));
  }
  svp::exec::throw_if_cancelled(cancellation, "embed.text_batch before output");
  return succeeded(spec, environment, records, vectors, embedded);
}

}  // namespace svp::vision::tasks::detail
