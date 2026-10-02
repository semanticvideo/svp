#include "engine/ocr_frame_batch_plan.hpp"

#include "engine/build_task_graph.hpp"
#include "engine/vision_lane_settings.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <stdexcept>

namespace svp::builder::engine {
namespace {

// The source is passed to ffmpeg as-is; its container is ffmpeg's business,
// so the reference names no particular one.
constexpr const char* kSourceMediaType = "application/octet-stream";

}  // namespace

std::optional<OcrWorkPlan> plan_ocr_work(const OcrWorkPlanInputs& inputs) {
  if (!inputs.stage_plan.run_package_skeleton || !inputs.model_runtime_available) {
    return std::nullopt;
  }
  const svp::package::VisionLaneSettings settings = make_vision_lane_settings(
      inputs.options, inputs.media_plan, inputs.media_plan_json, {}, inputs.thread_plan,
      inputs.model_runtime_available);
  const svp::package::SpatialProgressCallback no_progress;
  const svp::vision::OcrGenerationOptions ocr =
      svp::package::make_vision_ocr_options(settings, nullptr, no_progress);
  std::optional<svp::vision::OcrSamplePlan> samples = svp::vision::plan_ocr_samples(ocr);
  if (!samples || samples->samples.empty()) {
    return std::nullopt;
  }
  if (!svp::vision::ffmpeg_executable_available(ocr.ffmpeg_path)) {
    return std::nullopt;
  }
  std::optional<std::string> ffmpeg_build =
      svp::vision::tasks::cached_ffmpeg_build_identity(ocr.ffmpeg_path);
  if (!ffmpeg_build) {
    return std::nullopt;
  }
  OcrWorkPlan work;
  work.samples = std::move(*samples);
  work.pp_ocr = svp::vision::make_ocr_pp_ocr_options(ocr);
  try {
    work.model_refs = svp::vision::tasks::ocr_frame_batch_model_refs(work.pp_ocr);
  } catch (const std::exception&) {
    // No verifiable PP-OCR bundles: the whole stage reports its own blocker.
    return std::nullopt;
  }
  work.ffmpeg_build = std::move(*ffmpeg_build);
  work.source = svp::exec::ArtifactRef{
      .blake3 = inputs.source_blake3,
      .bytes = inputs.source_bytes,
      .media_type = kSourceMediaType,
      .role = std::string(svp::vision::tasks::kOcrFrameBatchSourceRole)};
  work.source_path = inputs.options.source_path;
  return work;
}

OcrFrameBatchPlan make_ocr_frame_batch_plan_from_batches(
    OcrWorkPlan work, std::vector<svp::vision::OcrSampleBatch> batches,
    const svp::vision::OcrBatchPolicy& policy, const std::string& build_session_id,
    const std::vector<std::string>& depends_on) {
  OcrFrameBatchPlan plan;
  plan.work = std::move(work);
  plan.batches = std::move(batches);
  const svp::vision::tasks::OcrFrameBatchTaskInputs inputs{
      .build_session_id = build_session_id,
      .depends_on = depends_on,
      .source = plan.work.source,
      .model_refs = plan.work.model_refs,
      .pp_ocr = plan.work.pp_ocr,
      .ffmpeg_build = plan.work.ffmpeg_build,
      .batch_policy = policy,
  };
  plan.nodes.reserve(plan.batches.size());
  for (const svp::vision::OcrSampleBatch& batch : plan.batches) {
    plan.nodes.push_back(svp::exec::TaskNode{
        .spec = svp::vision::tasks::make_ocr_frame_batch_task_spec(inputs, plan.work.samples,
                                                                   batch),
        .order_key = svp::vision::tasks::ocr_frame_batch_order_key(batch)});
  }
  return plan;
}

OcrFrameBatchPlan make_ocr_frame_batch_plan(OcrWorkPlan work,
                                            const svp::vision::OcrBatchPolicy& policy,
                                            const std::string& build_session_id,
                                            const std::vector<std::string>& depends_on) {
  std::vector<svp::vision::OcrSampleBatch> batches =
      svp::vision::partition_ocr_samples(work.samples.samples.size(), policy);
  return make_ocr_frame_batch_plan_from_batches(std::move(work), std::move(batches), policy,
                                                build_session_id, depends_on);
}

std::vector<std::string> ocr_stage_dependencies(const std::vector<PlannedStageTask>& tasks) {
  for (const PlannedStageTask& task : tasks) {
    if (task.kind != StageTaskKind::ocr) {
      continue;
    }
    std::vector<std::string> ids;
    for (const StageTaskKind dependency : task.depends_on) {
      ids.emplace_back(stage_task_id(dependency));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
  }
  throw std::logic_error("the build plan has no OCR stage task");
}

svp::exec::TaskGraph make_build_task_graph(const std::vector<PlannedStageTask>& tasks,
                                           const std::string& build_session_id,
                                           const std::string& build_inputs_blake3,
                                           const OcrFrameBatchPlan* batches) {
  return make_split_build_task_graph(tasks, build_session_id, build_inputs_blake3,
                                     SplitStageTasks{.ocr_batches = batches});
}

std::optional<std::vector<svp::vision::OcrSampleBatch>> ocr_batches_from_task_ids(
    const std::vector<std::string>& task_ids, std::size_t sample_count) {
  // ocr_frame_batch_task_id(): "task.ocr.frame_batch.samples_<first>_<last>".
  static const std::regex kBatchId(R"(^task\.ocr\.frame_batch\.samples_(\d+)_(\d+)$)");
  std::map<std::uint64_t, std::uint64_t> by_first;
  for (const std::string& id : task_ids) {
    std::smatch match;
    if (!std::regex_match(id, match, kBatchId)) {
      continue;
    }
    const std::uint64_t first = std::stoull(match[1].str());
    const std::uint64_t last = std::stoull(match[2].str());
    if (last < first || !by_first.emplace(first, last - first + 1).second) {
      return std::nullopt;
    }
  }
  if (by_first.empty()) {
    return std::nullopt;
  }
  std::vector<svp::vision::OcrSampleBatch> batches;
  std::uint64_t next = 0;
  for (const auto& [first, count] : by_first) {
    if (first != next) {
      return std::nullopt;
    }
    batches.push_back(svp::vision::OcrSampleBatch{.first_ordinal = first, .count = count});
    next = first + count;
  }
  if (next != sample_count) {
    return std::nullopt;
  }
  return batches;
}

}  // namespace svp::builder::engine
