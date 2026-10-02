#include "engine/vision_work_dispatch.hpp"

#include "engine/subtask_run.hpp"

#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/tasks/depth_frame_batch_spec.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_spec.hpp"
#include "svp/vision/tasks/embed_text_batch_spec.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_crop_batch_spec.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <utility>

namespace svp::builder::engine {
namespace {

namespace tasks = svp::vision::tasks;

using Progress = std::function<void(std::size_t done, std::size_t total)>;

// What an encoded crop image adds beyond its pixels: JPEG headers with their
// quantization and Huffman tables, or PNG's signature, chunk headers, and
// zlib framing, are a few hundred bytes; 4 KiB bounds them. A crop's pixels
// never encode larger than raw RGB24 at the qualities the stage uses, so
// pixels x 3 plus this bounds an image for batch sizing.
constexpr std::uint64_t kEncodedImageOverheadBytes = 4 * 1024;
constexpr std::uint64_t kRgb24BytesPerPixel = 3;

std::optional<svp::exec::TaskModelRef> model_ref(const VisionDispatchSetup& setup,
                                                 const std::string& model_id) {
  const auto found =
      std::find_if(setup.model_refs.begin(), setup.model_refs.end(),
                   [&](const svp::exec::TaskModelRef& ref) { return ref.model_id == model_id; });
  if (found == setup.model_refs.end()) {
    return std::nullopt;
  }
  return *found;
}

std::optional<tasks::ItemBatchPolicy> batch_policy(const VisionDispatchSetup& setup,
                                                   std::string_view task_type) {
  const auto capacity = setup.capacity.find(task_type);
  if (capacity == setup.capacity.end()) {
    return std::nullopt;
  }
  tasks::ItemBatchPolicy policy;
  policy.estimated_seconds_per_item = capacity->second.seconds_per_item;
  return policy;
}

tasks::OnnxModelParameters onnx_model(const svp::vision::DispatchedModel& model) {
  return tasks::OnnxModelParameters{.model_id = model.model_id,
                                    .execution_provider = model.execution_provider,
                                    .threads = model.threads};
}

// Runs the nodes and reads every result with `read`, concatenating the
// per-task outcomes in task order. Reading errors mean a result does not
// match its spec: the stage cannot use it.
template <typename Outcome, typename Read>
std::vector<Outcome> run_and_read(const std::string& task_type, std::vector<svp::exec::TaskNode> nodes,
                                  const std::vector<tasks::ItemBatch>& batches,
                                  std::size_t item_count, const Progress& on_progress,
                                  const VisionDispatchSetup& setup,
                                  const svp::exec::TaskTypeRegistry& registry,
                                  svp::exec::TaskArtifactAccess& artifacts, Read read) {
  std::map<std::string, std::uint64_t> items_by_task;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    items_by_task.emplace(nodes[index].spec.task_id, batches[index].count);
  }
  std::size_t done = 0;
  if (on_progress) {
    on_progress(0, item_count);
  }
  const SubtaskRunRequest request{
      .task_type = task_type,
      .nodes = std::move(nodes),
      .on_committed =
          [&](const svp::exec::TaskNode& node) {
            done += items_by_task.at(node.spec.task_id);
            if (on_progress) {
              on_progress(done, item_count);
            }
          }};
  std::vector<svp::exec::CommittedResult> results = [&] {
    // This Mac's idle models go once the stage's tasks are done, whatever
    // the outcome; the stage does not reuse them.
    struct Release {
      const std::function<void()>& release;
      ~Release() {
        if (release) release();
      }
    } release{setup.release_idle_models};
    return run_subtasks(request, setup, registry, artifacts);
  }();
  std::vector<Outcome> outcomes;
  outcomes.reserve(item_count);
  for (std::size_t index = 0; index < results.size(); ++index) {
    try {
      std::vector<Outcome> part =
          read(request.nodes[index].spec, results[index].result.outputs, results[index].payloads);
      std::move(part.begin(), part.end(), std::back_inserter(outcomes));
      // Each task's bytes go as soon as its outcomes are read, so the
      // stage never holds a payload and its outcome copy for every task.
      std::vector<svp::exec::FramePayload>().swap(results[index].payloads);
    } catch (const std::exception& error) {
      throw svp::vision::DispatchedWorkError(task_type + ": " + error.what());
    }
  }
  if (outcomes.size() != item_count) {
    throw svp::vision::DispatchedWorkError(task_type + ": " + std::to_string(outcomes.size()) +
                                           " outcomes for " + std::to_string(item_count) +
                                           " items");
  }
  return outcomes;
}

struct PlannedTasks {
  std::vector<svp::exec::TaskNode> nodes;
  std::vector<tasks::ItemBatch> batches;
};

// Plans a stage's tasks with `plan`; nullopt, after saying why, when they
// cannot be planned (for example settings no task schema accepts): the stage
// then does the work itself, exactly as a local build does.
template <typename Plan>
std::optional<PlannedTasks> plan_or_skip(const VisionDispatchSetup& setup,
                                         const std::string& task_type, Plan plan) {
  try {
    return plan();
  } catch (const std::exception& error) {
    if (setup.report) {
      std::cerr << "svp-builder: warning: " << task_type
                << " work runs in its stage on this Mac: " << error.what() << "\n";
    }
    return std::nullopt;
  }
}

// A dispatcher that cannot deliver must fail the build, never become a
// stage blocker: the stages rethrow only DispatchedWorkError and turn any
// other exception into a blocker (svp/vision/dispatched_work.hpp). So every
// exception leaving a dispatcher (scheduler, executor, transport, memory)
// leaves it as DispatchedWorkError, its message kept.
template <typename Function>
Function guarded(std::string task_type, Function inner) {
  return [task_type = std::move(task_type), inner = std::move(inner)](const auto&... arguments) {
    try {
      return inner(arguments...);
    } catch (const svp::vision::DispatchedWorkError&) {
      throw;
    } catch (const std::exception& error) {
      throw svp::vision::DispatchedWorkError(task_type + ": " + error.what());
    } catch (...) {
      throw svp::vision::DispatchedWorkError(task_type + ": unknown failure");
    }
  };
}

}  // namespace

svp::package::VisionWorkDispatch make_vision_work_dispatch(
    std::shared_ptr<const VisionDispatchSetup> setup, const svp::exec::TaskTypeRegistry& registry,
    svp::exec::TaskArtifactAccess& artifacts) {
  svp::package::VisionWorkDispatch dispatch;

  dispatch.evidence_crops =
      [setup, &registry, &artifacts](const std::vector<svp::vision::EvidenceCropJob>& jobs,
                                     const svp::vision::PpOcrOptions& roi_options,
                                     const Progress& on_progress)
      -> std::optional<std::vector<svp::vision::EvidenceCropJobOutcome>> {
    const std::string type(tasks::kOcrCropBatchTaskType);
    const std::optional<tasks::ItemBatchPolicy> policy = batch_policy(*setup, type);
    const auto det = model_ref(*setup, roi_options.detector_model_id);
    const auto rec = model_ref(*setup, roi_options.recognizer_model_id);
    if (!policy || !det || !rec || jobs.empty()) {
      return std::nullopt;
    }
    const tasks::OcrCropBatchTaskInputs inputs{.build_session_id = setup->build_session_id,
                                               .depends_on = {},
                                               .source = setup->source,
                                               .model_refs = {*det, *rec},
                                               .pp_ocr = roi_options,
                                               .ffmpeg_build = setup->ffmpeg_build,
                                               .batch_policy = *policy};
    std::optional<PlannedTasks> planned = plan_or_skip(*setup, type, [&] {
      PlannedTasks tasks_of_stage;
      tasks_of_stage.batches =
          tasks::partition_items(jobs.size(), *policy, [&jobs](std::size_t index) {
            return tasks::ItemBytes{
                .parameter_bytes = tasks::ocr_crop_job_parameter_bytes(jobs[index]),
                .result_bytes = svp::vision::evidence_crop_image_pixels(jobs[index]) *
                                        kRgb24BytesPerPixel +
                                kEncodedImageOverheadBytes};
          });
      for (const tasks::ItemBatch& batch : tasks_of_stage.batches) {
        tasks_of_stage.nodes.push_back(
            {.spec = tasks::make_ocr_crop_batch_task_spec(inputs, jobs, batch),
             .order_key = tasks::ocr_crop_batch_order_key(batch)});
      }
      return tasks_of_stage;
    });
    if (!planned) {
      return std::nullopt;
    }
    return run_and_read<svp::vision::EvidenceCropJobOutcome>(
        type, std::move(planned->nodes), planned->batches, jobs.size(), on_progress, *setup,
        registry, artifacts, tasks::read_ocr_crop_batch_output);
  };

  dispatch.text_embeddings =
      [setup, &registry, &artifacts](const std::vector<svp::vision::TextEmbeddingItem>& items,
                                     const svp::vision::DispatchedModel& model,
                                     std::uint32_t embedding_dim, const Progress& on_progress)
      -> std::optional<std::vector<svp::vision::TextEmbeddingOutcome>> {
    const std::string type(tasks::kEmbedTextBatchTaskType);
    const std::optional<tasks::ItemBatchPolicy> policy = batch_policy(*setup, type);
    const auto ref = model_ref(*setup, model.model_id);
    if (!policy || !ref || items.empty()) {
      return std::nullopt;
    }
    const tasks::EmbedTextBatchTaskInputs inputs{.build_session_id = setup->build_session_id,
                                                 .depends_on = {},
                                                 .model_ref = *ref,
                                                 .model = onnx_model(model),
                                                 .embedding_dim = embedding_dim,
                                                 .batch_policy = *policy};
    std::optional<PlannedTasks> planned = plan_or_skip(*setup, type, [&] {
      PlannedTasks tasks_of_stage;
      tasks_of_stage.batches =
          tasks::partition_items(items.size(), *policy, [&](std::size_t index) {
            return tasks::ItemBytes{
                .parameter_bytes = tasks::embed_text_item_parameter_bytes(
                    {.ordinal = index, .item = items[index]}),
                .result_bytes = std::uint64_t{embedding_dim} * sizeof(float)};
          });
      for (const tasks::ItemBatch& batch : tasks_of_stage.batches) {
        tasks_of_stage.nodes.push_back(
            {.spec = tasks::make_embed_text_batch_task_spec(inputs, items, batch),
             .order_key = tasks::embed_text_batch_order_key(batch)});
      }
      return tasks_of_stage;
    });
    if (!planned) {
      return std::nullopt;
    }
    return run_and_read<svp::vision::TextEmbeddingOutcome>(
        type, std::move(planned->nodes), planned->batches, items.size(), on_progress, *setup,
        registry, artifacts, tasks::read_embed_text_batch_output);
  };

  dispatch.keyframe_embeddings =
      [setup, &registry, &artifacts](const std::vector<svp::vision::KeyframeEmbeddingItem>& items,
                                     const svp::vision::DispatchedModel& model,
                                     std::uint32_t embedding_dim, const Progress& on_progress)
      -> std::optional<std::vector<svp::vision::KeyframeEmbeddingOutcome>> {
    const std::string type(tasks::kEmbedKeyframeBatchTaskType);
    const std::optional<tasks::ItemBatchPolicy> policy = batch_policy(*setup, type);
    const auto ref = model_ref(*setup, model.model_id);
    if (!policy || !ref || items.empty()) {
      return std::nullopt;
    }
    const tasks::EmbedKeyframeBatchTaskInputs inputs{
        .build_session_id = setup->build_session_id,
        .depends_on = {},
        .source = setup->source,
        .model_ref = *ref,
        .model = onnx_model(model),
        .embedding_dim = embedding_dim,
        .ffmpeg_build = setup->ffmpeg_build,
        .batch_policy = *policy};
    std::optional<PlannedTasks> planned = plan_or_skip(*setup, type, [&] {
      PlannedTasks tasks_of_stage;
      tasks_of_stage.batches =
          tasks::partition_items(items.size(), *policy, [&](std::size_t index) {
            return tasks::ItemBytes{
                .parameter_bytes = tasks::embed_keyframe_parameter_bytes(
                    {.ordinal = index, .item = items[index]}),
                .result_bytes = std::uint64_t{embedding_dim} * sizeof(float)};
          });
      for (const tasks::ItemBatch& batch : tasks_of_stage.batches) {
        tasks_of_stage.nodes.push_back(
            {.spec = tasks::make_embed_keyframe_batch_task_spec(inputs, items, batch),
             .order_key = tasks::embed_keyframe_batch_order_key(batch)});
      }
      return tasks_of_stage;
    });
    if (!planned) {
      return std::nullopt;
    }
    return run_and_read<svp::vision::KeyframeEmbeddingOutcome>(
        type, std::move(planned->nodes), planned->batches, items.size(), on_progress, *setup,
        registry, artifacts, tasks::read_embed_keyframe_batch_output);
  };

  dispatch.depth_frames =
      [setup, &registry, &artifacts](const std::vector<svp::vision::ColorRasterFrame>& frames,
                                     const svp::vision::DispatchedModel& model,
                                     const Progress& on_progress)
      -> std::optional<std::vector<svp::vision::DepthFrameOutcome>> {
    const std::string type(tasks::kDepthFrameBatchTaskType);
    const std::optional<tasks::ItemBatchPolicy> policy = batch_policy(*setup, type);
    const auto ref = model_ref(*setup, model.model_id);
    if (!policy || !ref || frames.empty()) {
      return std::nullopt;
    }
    const tasks::DepthFrameBatchTaskInputs inputs{.build_session_id = setup->build_session_id,
                                                  .depends_on = {},
                                                  .source = setup->source,
                                                  .model_ref = *ref,
                                                  .model = onnx_model(model),
                                                  .ffmpeg_build = setup->ffmpeg_build,
                                                  .batch_policy = *policy};
    std::optional<PlannedTasks> planned = plan_or_skip(*setup, type, [&] {
      std::vector<tasks::DepthFrameItem> items;
      items.reserve(frames.size());
      for (std::size_t index = 0; index < frames.size(); ++index) {
        items.push_back(tasks::depth_frame_item(frames[index], index));
      }
      PlannedTasks tasks_of_stage;
      tasks_of_stage.batches =
          tasks::partition_items(items.size(), *policy, [&](std::size_t index) {
            return tasks::ItemBytes{
                .parameter_bytes = tasks::depth_frame_parameter_bytes(items[index]),
                .result_bytes = tasks::depth_frame_field_bytes(items[index])};
          });
      for (const tasks::ItemBatch& batch : tasks_of_stage.batches) {
        tasks_of_stage.nodes.push_back(
            {.spec = tasks::make_depth_frame_batch_task_spec(inputs, items, batch),
             .order_key = tasks::depth_frame_batch_order_key(batch)});
      }
      return tasks_of_stage;
    });
    if (!planned) {
      return std::nullopt;
    }
    return run_and_read<svp::vision::DepthFrameOutcome>(
        type, std::move(planned->nodes), planned->batches, frames.size(), on_progress, *setup,
        registry, artifacts, tasks::read_depth_frame_batch_output);
  };
  dispatch.evidence_crops =
      guarded(std::string(tasks::kOcrCropBatchTaskType), std::move(dispatch.evidence_crops));
  dispatch.text_embeddings =
      guarded(std::string(tasks::kEmbedTextBatchTaskType), std::move(dispatch.text_embeddings));
  dispatch.keyframe_embeddings = guarded(std::string(tasks::kEmbedKeyframeBatchTaskType),
                                         std::move(dispatch.keyframe_embeddings));
  dispatch.depth_frames =
      guarded(std::string(tasks::kDepthFrameBatchTaskType), std::move(dispatch.depth_frames));
  return dispatch;
}

}  // namespace svp::builder::engine
