#pragma once

// Every dispatched vision task type a runtime serves (svp/vision/
// dispatched_work.hpp): ocr.crop_batch (ocr_crop_batch_task.hpp) and
// embed.text_batch, embed.keyframe_batch, depth.frame_batch
// (onnx_task_registration.hpp).

#include "svp/exec/task_registry.hpp"
#include "svp/vision/tasks/dispatched_task_environment.hpp"
#include "svp/vision/tasks/pp_ocr_session_pool.hpp"

#include <functional>
#include <memory>

namespace svp::vision::tasks {

// `sessions` is the PP-OCR pool crop tasks take sessions from (null: a pool of
// their own). Returns a function that frees the ONNX models the embedding and
// depth types keep loaded between tasks (onnx_task_registration.hpp).
std::function<void()> register_dispatched_vision_tasks(
    svp::exec::TaskTypeRegistry& registry, const DispatchedTaskEnvironment& environment,
    std::shared_ptr<PpOcrSessionPool> sessions = nullptr);

}  // namespace svp::vision::tasks
