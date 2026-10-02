#include "svp/vision/tasks/dispatched_vision_tasks.hpp"

#include "svp/vision/tasks/ocr_crop_batch_task.hpp"
#include "svp/vision/tasks/onnx_task_registration.hpp"

namespace svp::vision::tasks {

std::function<void()> register_dispatched_vision_tasks(
    svp::exec::TaskTypeRegistry& registry, const DispatchedTaskEnvironment& environment,
    std::shared_ptr<PpOcrSessionPool> sessions) {
  register_ocr_crop_batch_task(registry, environment, std::move(sessions));
  return register_onnx_vision_tasks(registry, environment);
}

}  // namespace svp::vision::tasks
