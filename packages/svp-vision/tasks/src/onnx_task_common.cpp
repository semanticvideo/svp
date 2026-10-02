#include "onnx_task_common.hpp"

#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/model_refs.hpp"

namespace svp::vision::tasks::detail {

OnnxTaskStart start_onnx_task(const svp::exec::TaskSpec& spec, std::string_view task_type,
                              const OnnxModelParameters& model, bool with_tokenizer,
                              const DispatchedTaskEnvironment& environment,
                              OnnxModelPool& models, const CouldNotStart& could_not_start) {
  OnnxTaskStart start;
  if (const auto problem = single_model_ref_problem(spec, model.model_id)) {
    start.result = failed_result(spec, task_type, "invalid_model_refs", *problem, false);
    return start;
  }
  std::filesystem::path cache = environment.model_cache_root;
  if (environment.model_cache_for) {
    try {
      cache = environment.model_cache_for(spec);
    } catch (const std::exception& error) {
      start.result = failed_result(spec, task_type, "model_unavailable", error.what(), true);
      return start;
    }
  }
  start.model = models.acquire(OnnxModelRequest{.model_cache_root = cache,
                                                .model_id = model.model_id,
                                                .execution_provider = model.execution_provider,
                                                .threads = model.threads,
                                                .with_tokenizer = with_tokenizer});
  const LoadedOnnxModel& loaded = start.model->model();
  if (!loaded.problem.empty()) {
    start.result = could_not_start("model_unavailable", loaded.problem);
    return start;
  }
  if (const auto mismatch =
          loaded_bundle_mismatch(spec, model.model_id, loaded.manifest->bundle_blake3.hex_value())) {
    start.result = failed_result(spec, task_type, "model_mismatch", *mismatch, true);
  }
  return start;
}

std::optional<svp::exec::TaskResult> check_ffmpeg_build(
    const svp::exec::TaskSpec& spec, std::string_view task_type, const std::string& ffmpeg_build,
    const DispatchedTaskEnvironment& environment, const CouldNotStart& could_not_start) {
  const std::optional<std::string> decoder = cached_ffmpeg_build_identity(environment.ffmpeg_path);
  if (!decoder) {
    return could_not_start("decode_unavailable",
                           "cannot run ffmpeg at " + environment.ffmpeg_path.string());
  }
  if (*decoder != ffmpeg_build) {
    return failed_result(spec, task_type, "decoder_mismatch",
                         "ffmpeg at " + environment.ffmpeg_path.string() + " is build " +
                             *decoder + ", the coordinator uses " + ffmpeg_build,
                         true);
  }
  return std::nullopt;
}

}  // namespace svp::vision::tasks::detail
