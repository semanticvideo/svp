#pragma once

// Registration of the dispatched vision task types that run one ONNX model:
// embed.text_batch, embed.keyframe_batch, depth.frame_batch. They share one
// model pool per registry (a runtime's sessions are reused across its tasks).

#include "svp/exec/task_registry.hpp"
#include "svp/vision/tasks/dispatched_task_environment.hpp"

#include <functional>

namespace svp::vision::tasks {

// Registers embed.text_batch version 1. For each item, in parameter order,
// runs embed_text_item (svp/vision/text_embedding_work.hpp) with the text
// model and its tokenizer, and returns one record per item
// (kEmbedTextRecordsRole: ordinal, and either the vector's byte range or the
// stage's blocker text as `error`) and the vectors (kEmbedTextVectorsRole).
//
// Registers embed.keyframe_batch version 1. For each keyframe, decodes its
// frame from the `source` input and runs embed_keyframe
// (svp/vision/keyframe_embedding_work.hpp), returning one record per
// keyframe (ordinal, embedded, and the vector's byte range when embedded)
// and the vectors.
//
// Registers depth.frame_batch version 1. For each canonical frame, decodes
// it from the `source` input, checks its pixels against the coordinator's
// (pixels_blake3), and runs infer_depth_frame_outcome
// (svp/vision/depth_frame_work.hpp), returning one record per frame
// (ordinal, status, and the uint16 field's byte range when ok) and the
// fields. A frame that does not decode to the coordinator's pixels is
// status `unavailable`; the stage runs it itself.
//
// For all three: model_refs must name exactly the parameters' model
// (permanent otherwise); a model that cannot load here (or ffmpeg that
// cannot run) is a retryable failure on a worker and every item failed on
// the coordinator (environment.record_start_failures); a loaded bundle or
// ffmpeg build other than the spec's is a retryable model_mismatch or
// decoder_mismatch. Cancellation is checked between items.
//
// Returns a function that frees the models these registrations keep loaded
// between tasks (a runtime calls it once a stage's tasks are done, so later
// stages do not run beside models nothing will use again).
[[nodiscard]] std::function<void()> register_onnx_vision_tasks(
    svp::exec::TaskTypeRegistry& registry, DispatchedTaskEnvironment environment);

}  // namespace svp::vision::tasks
