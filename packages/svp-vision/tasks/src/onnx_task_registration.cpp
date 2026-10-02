#include "svp/vision/tasks/onnx_task_registration.hpp"

#include "depth_frame_batch_execution.hpp"
#include "embed_keyframe_batch_execution.hpp"
#include "embed_text_batch_execution.hpp"
#include "onnx_model_pool.hpp"

#include "svp/vision/tasks/depth_frame_batch_parameters.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_parameters.hpp"
#include "svp/vision/tasks/embed_text_batch_parameters.hpp"

#include <memory>

namespace svp::vision::tasks {

std::function<void()> register_onnx_vision_tasks(svp::exec::TaskTypeRegistry& registry,
                                                 DispatchedTaskEnvironment environment) {
  auto models = std::make_shared<detail::OnnxModelPool>();
  auto shared_environment = std::make_shared<const DispatchedTaskEnvironment>(std::move(environment));
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kEmbedTextBatchTaskType),
      .version = kEmbedTextBatchTaskTypeVersion,
      .validate_parameters = validate_embed_text_batch_parameters,
      .execute = [shared_environment, models](const svp::exec::TaskSpec& spec,
                                              const svp::exec::ResolvedInputs& inputs,
                                              const svp::exec::CancellationToken& cancellation) {
        return detail::execute_embed_text_batch(spec, inputs, cancellation, *shared_environment,
                                                *models);
      }});
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kEmbedKeyframeBatchTaskType),
      .version = kEmbedKeyframeBatchTaskTypeVersion,
      .validate_parameters = validate_embed_keyframe_batch_parameters,
      .execute = [shared_environment, models](const svp::exec::TaskSpec& spec,
                                              const svp::exec::ResolvedInputs& inputs,
                                              const svp::exec::CancellationToken& cancellation) {
        return detail::execute_embed_keyframe_batch(spec, inputs, cancellation,
                                                    *shared_environment, *models);
      }});
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kDepthFrameBatchTaskType),
      .version = kDepthFrameBatchTaskTypeVersion,
      .validate_parameters = validate_depth_frame_batch_parameters,
      .execute = [shared_environment, models](const svp::exec::TaskSpec& spec,
                                              const svp::exec::ResolvedInputs& inputs,
                                              const svp::exec::CancellationToken& cancellation) {
        return detail::execute_depth_frame_batch(spec, inputs, cancellation, *shared_environment,
                                                 *models);
      }});
  return [models] { models->clear_idle(); };
}

}  // namespace svp::vision::tasks
