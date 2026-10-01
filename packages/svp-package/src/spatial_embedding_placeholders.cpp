#include "svp/package/spatial_embedding_placeholders.hpp"

#include "svp/package/vision_lane_stages.hpp"
#include "vision_lane_files.hpp"

#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svp::package {
namespace {

std::string string_value(const nlohmann::json& record, const char* key) {
  const auto iterator = record.find(key);
  if (iterator == record.end() || !iterator->is_string()) {
    return {};
  }
  return iterator->get<std::string>();
}

void append_processor_records_to_path(
    const std::filesystem::path& processors_path,
    const std::vector<nlohmann::json>& new_processors) {
  std::map<std::string, nlohmann::json> processors_by_id;
  for (const nlohmann::json& processor : detail::read_jsonl_records(processors_path)) {
    const std::string id = string_value(processor, "id");
    if (!id.empty()) {
      processors_by_id[id] = processor;
    }
  }
  for (const nlohmann::json& processor : new_processors) {
    const std::string id = string_value(processor, "id");
    if (!id.empty()) {
      processors_by_id[id] = processor;
    }
  }
  std::vector<nlohmann::json> all_processors;
  all_processors.reserve(processors_by_id.size());
  for (const auto& [id, processor] : processors_by_id) {
    all_processors.push_back(processor);
  }
  detail::write_jsonl_records(processors_path, all_processors);
}

void collect_or_append_processor_records(
    const std::filesystem::path& processors_path,
    const std::vector<nlohmann::json>& new_processors,
    std::vector<nlohmann::json>* processor_records) {
  if (processor_records != nullptr) {
    processor_records->insert(processor_records->end(),
                              new_processors.begin(),
                              new_processors.end());
    return;
  }
  append_processor_records_to_path(processors_path, new_processors);
}

}  // namespace

SpatialEmbeddingPlaceholderSummary write_spatial_and_embedding_placeholders(
    const std::filesystem::path& staging_dir,
    bool model_runtime_available,
    const nlohmann::json& media_plan_json,
    const std::filesystem::path& model_cache_root,
    const svp::media::MediaIngestPlan* media_plan,
    const std::filesystem::path& ffmpeg_path,
    svp::vision::FrameCatalog* frame_catalog,
    SpatialProgressCallback on_progress,
    const svp::vision::InferencePerformanceOptions& performance,
    const svp::models::ThreadPlan& thread_plan,
    std::string_view visual_tracking_quality,
    bool serial_model_stages,
    std::vector<nlohmann::json>* processor_records) {
  const VisionLaneSettings settings{
      .staging_dir = staging_dir,
      .model_runtime_available = model_runtime_available,
      .media_plan_json = media_plan_json,
      .model_cache_root = model_cache_root,
      .media_plan = media_plan,
      .ffmpeg_path = ffmpeg_path,
      .performance = performance,
      .thread_plan = thread_plan,
      .visual_tracking_quality = std::string(visual_tracking_quality),
  };
  const std::filesystem::path processors_path =
      staging_dir / "provenance" / "processors.jsonl";

  // Decode the canonical frames once and share them across depth and OCR.
  const svp::vision::DecodedCanonicalFrames frames =
      decode_vision_lane_canonical_frames(settings, frame_catalog);

  // Depth runs beside OCR and embeddings unless model stages are serial.
  std::optional<std::future<VisionDepthStageResult>> depth_future;
  std::optional<VisionDepthStageResult> depth;
  if (serial_model_stages || !model_runtime_available) {
    depth = run_vision_depth_stage(settings, frames, on_progress);
  } else {
    depth_future.emplace(std::async(std::launch::async, [&] {
      return run_vision_depth_stage(settings, frames, on_progress);
    }));
  }

  // OCR before embeddings, so text_observations.jsonl exists when embedding
  // generation reads it.
  const VisionOcrStageResult ocr =
      run_vision_ocr_stage(settings, frames, frame_catalog, on_progress);
  if (!ocr.processors.empty()) {
    collect_or_append_processor_records(processors_path, ocr.processors,
                                        processor_records);
  }
  const VisionTextEmbeddingStageResult embeddings =
      run_vision_text_embedding_stage(settings, on_progress);
  if (depth_future.has_value()) {
    depth = depth_future->get();
  }
  const VisionTrackingStageResult tracking =
      run_vision_tracking_stage(settings, frame_catalog, on_progress);

  VisionLaneOutcome outcome = combine_vision_lane_results(
      model_runtime_available, *depth, ocr, embeddings, tracking);
  collect_or_append_processor_records(
      processors_path, {depth->processor, embeddings.processor}, processor_records);
  return outcome.summary;
}

void merge_processor_records(const std::filesystem::path& processors_path,
                             const std::vector<nlohmann::json>& new_processors) {
  append_processor_records_to_path(processors_path, new_processors);
}

nlohmann::json spatial_embedding_placeholder_summary_to_json(
    const SpatialEmbeddingPlaceholderSummary& summary) {
  return {
      {"depth_index_written", summary.depth_index_written != 0},
      {"depth_blocks_written", summary.depth_blocks_written != 0},
      {"depth_placeholder_written", summary.depth_placeholder_written != 0},
      {"depth_generation_run", summary.depth_generation_run != 0},
      {"depth_model_available", summary.depth_model_available != 0},
      {"depth_model_verified", summary.depth_model_verified != 0},
      {"depth_frame_input_available", summary.depth_frame_input_available != 0},
      {"masks_index_written", summary.masks_index_written != 0},
      {"masks_blocks_written", summary.masks_blocks_written != 0},
      {"embedding_sets_written", summary.embedding_sets_written != 0},
      {"embeddings_index_written", summary.embeddings_index_written != 0},
      {"embeddings_blocks_written", summary.embeddings_blocks_written != 0},
      {"embedding_generation_run", summary.embedding_generation_run != 0},
      {"embedding_model_available", summary.embedding_model_available != 0},
      {"model_runtime_available", summary.model_runtime_available != 0},
      {"provenance_records_added", summary.provenance_records_added},
      {"ocr_available", summary.ocr_available != 0},
      {"ocr_frame_input_available", summary.ocr_frame_input_available != 0},
      {"ocr_detection_run", summary.ocr_detection_run != 0},
      {"ocr_recognition_run", summary.ocr_recognition_run != 0},
      {"text_observation_count", summary.text_observation_count},
      {"numeric_value_count", summary.numeric_value_count},
      {"depth_generation_detail", summary.depth_generation_detail},
      {"embedding_generation_detail", summary.embedding_generation_detail},
      {"ocr_generation_detail", summary.ocr_generation_detail},
  };
}

}  // namespace svp::package
