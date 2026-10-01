#include "vision_stage_progress.hpp"

namespace svp::builder {
namespace {

ProgressStageId spatial_stage_id(const std::string& stage) {
  if (stage == "ocr_evidence_crops") return ProgressStageId::ocr_evidence_crops;
  if (stage == "depth") return ProgressStageId::depth;
  if (stage == "text_embeddings") return ProgressStageId::text_embeddings;
  if (stage == "visual_tracking") return ProgressStageId::visual_tracking;
  if (stage == "visual_embeddings") return ProgressStageId::visual_embeddings;
  return ProgressStageId::ocr;
}

std::string spatial_stage_unit(const std::string& stage) {
  if (stage == "ocr") return "frames";
  if (stage == "ocr_evidence_crops") return "steps";
  return "items";
}

}  // namespace

VisionStageProgress::VisionStageProgress(BuildPipelineContext& context)
    : context_(context) {}

svp::package::SpatialProgressCallback VisionStageProgress::callback() {
  return [this](const char* stage, std::size_t current, std::size_t total,
                const char* message) { report(stage, current, total, message); };
}

void VisionStageProgress::report(const char* stage, std::size_t current,
                                 std::size_t total, const char* message) {
  const std::string stage_name(stage == nullptr ? "" : stage);
  const ProgressStageId stage_id = spatial_stage_id(stage_name);
  bool should_start = false;
  bool should_complete = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    should_start = started_.insert(stage_name).second;
    should_complete = current > 0 && current >= total && total > 0 &&
                      completed_.insert(stage_name).second;
  }
  if (should_start) {
    emit_stage_started(context_, stage_id);
  }
  if (total > 0) {
    emit_stage_progress(context_, stage_id, static_cast<std::uint64_t>(current),
                        static_cast<std::uint64_t>(total),
                        spatial_stage_unit(stage_name),
                        message == nullptr ? "" : message);
  }
  if (should_complete) {
    emit_stage_completed(context_, stage_id);
  }
}

void VisionStageProgress::finish() {
  for (const auto& stage : {"ocr", "ocr_evidence_crops", "depth", "text_embeddings",
                            "visual_tracking", "visual_embeddings"}) {
    bool complete = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      complete = started_.count(stage) != 0 && completed_.insert(stage).second;
    }
    if (complete) {
      emit_stage_completed(context_, spatial_stage_id(stage));
    }
  }
}

}  // namespace svp::builder
