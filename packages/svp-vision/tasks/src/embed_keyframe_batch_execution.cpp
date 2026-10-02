#include "embed_keyframe_batch_execution.hpp"

#include "numeric_bytes.hpp"
#include "onnx_task_common.hpp"
#include "task_results.hpp"

#include "svp/vision/keyframe_embedding_work.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_parameters.hpp"

namespace svp::vision::tasks::detail {
namespace {

svp::exec::TaskResult succeeded(const svp::exec::TaskSpec& spec,
                                const DispatchedTaskEnvironment& environment,
                                const std::vector<nlohmann::json>& records,
                                const std::vector<std::byte>& vectors, std::size_t embedded) {
  return succeeded_result(spec, environment.write_output, records, vectors,
                          kEmbedKeyframeRecordsRole, kEmbedKeyframeVectorsRole,
                          {{"embedded", embedded}, {"keyframes", records.size()}});
}

}  // namespace

svp::exec::TaskResult execute_embed_keyframe_batch(
    const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
    const svp::exec::CancellationToken& cancellation,
    const DispatchedTaskEnvironment& environment, OnnxModelPool& models) {
  svp::exec::throw_if_cancelled(cancellation, "embed.keyframe_batch start");
  const EmbedKeyframeBatchParameters parameters =
      embed_keyframe_batch_parameters_from_json(spec.parameters);
  if (spec.inputs.size() != 1 ||
      !spec.inputs.contains(std::string(kEmbedKeyframeBatchSourceInput))) {
    return failed_result(spec, kEmbedKeyframeBatchTaskType, "invalid_inputs",
                         "inputs must be exactly `" +
                             std::string(kEmbedKeyframeBatchSourceInput) + "`",
                         false);
  }
  const CouldNotStart could_not_start = [&](std::string code, std::string reason) {
    if (!environment.record_start_failures) {
      return failed_result(spec, kEmbedKeyframeBatchTaskType, std::move(code),
                           std::move(reason), true);
    }
    // The stage embeds every keyframe itself (svp/vision/dispatched_work.hpp).
    std::vector<nlohmann::json> records;
    for (const OrderedKeyframeItem& keyframe : parameters.keyframes) {
      records.push_back({{"embedded", false}, {"ordinal", keyframe.ordinal}});
    }
    return succeeded(spec, environment, records, {}, 0);
  };
  if (auto ended = check_ffmpeg_build(spec, kEmbedKeyframeBatchTaskType, parameters.ffmpeg_build,
                                      environment, could_not_start)) {
    return std::move(*ended);
  }
  OnnxTaskStart start = start_onnx_task(spec, kEmbedKeyframeBatchTaskType, parameters.model,
                                        false, environment, models, could_not_start);
  if (start.result) {
    return std::move(*start.result);
  }
  const LoadedOnnxModel& model = start.model->model();
  const std::filesystem::path& source =
      inputs.at(std::string(kEmbedKeyframeBatchSourceInput)).path;

  std::vector<nlohmann::json> records;
  std::vector<std::byte> vectors;
  std::size_t embedded = 0;
  for (const OrderedKeyframeItem& keyframe : parameters.keyframes) {
    svp::exec::throw_if_cancelled(cancellation, "embed.keyframe_batch between keyframes");
    const KeyframeEmbeddingOutcome outcome = embed_keyframe(
        model.session, environment.ffmpeg_path, source, keyframe.item, parameters.embedding_dim);
    if (!outcome.embedded) {
      records.push_back({{"embedded", false}, {"ordinal", keyframe.ordinal}});
      continue;
    }
    ++embedded;
    nlohmann::json record = append_data(vectors, values_to_bytes(outcome.vector));
    record["embedded"] = true;
    record["ordinal"] = keyframe.ordinal;
    records.push_back(std::move(record));
  }
  svp::exec::throw_if_cancelled(cancellation, "embed.keyframe_batch before output");
  return succeeded(spec, environment, records, vectors, embedded);
}

}  // namespace svp::vision::tasks::detail
