#include "svp/package/vision_lane_stages.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/package/entity_writer.hpp"
#include "svp/package/visual_entity_artifact_writer.hpp"
#include "svp/vision/depth_generation.hpp"
#include "svp/vision/embedding_generation.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_frame_batch_reduction.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_tracker.hpp"
#include "vision_lane_files.hpp"
#include "vision_lane_placeholders.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace svp::package {
namespace {

// Raster used for bbox normalization and depth when the media plan JSON has no
// canonical_raster (only the legacy placeholder path without a plan). The
// values are the long-standing fallback of the package vision stage, kept so
// that path writes exactly what it always wrote.
constexpr std::uint32_t kFallbackRasterWidth = 640;
constexpr std::uint32_t kFallbackRasterHeight = 360;

// OCR evidence-crop policy of package builds: one bounded crop per text
// observation, never encoded below this JPEG quality.
constexpr const char* kEvidenceCropCoveragePolicy = "one_per_observation";
constexpr int kEvidenceCropMinJpegQuality = 50;

struct RasterSize {
  std::uint32_t width = kFallbackRasterWidth;
  std::uint32_t height = kFallbackRasterHeight;
};

RasterSize canonical_raster(const nlohmann::json& media_plan_json) {
  RasterSize raster;
  if (!media_plan_json.empty() && media_plan_json.contains("canonical_raster")) {
    const auto& value = media_plan_json["canonical_raster"];
    if (value.contains("width") && value["width"].is_number()) {
      raster.width = value["width"].get<std::uint32_t>();
    }
    if (value.contains("height") && value["height"].is_number()) {
      raster.height = value["height"].get<std::uint32_t>();
    }
  }
  return raster;
}

void attach_ocr_progress_callbacks(svp::vision::OcrGenerationOptions& ocr_opts,
                                   const SpatialProgressCallback& on_progress) {
  if (!on_progress) {
    return;
  }

  auto evidence_observation_total = std::make_shared<std::size_t>(0);
  ocr_opts.on_progress = [&on_progress](int current, int total) {
    on_progress("ocr",
                static_cast<std::size_t>(current),
                static_cast<std::size_t>(total),
                "");
  };
  ocr_opts.on_evidence_crop_progress =
      [on_progress, evidence_observation_total](std::size_t current,
                                                std::size_t total) {
        if (total == 0) {
          return;
        }
        *evidence_observation_total = total;
        on_progress("ocr_evidence_crops",
                    current,
                    *evidence_observation_total * 2,
                    "extracting evidence crops");
      };
  ocr_opts.on_evidence_roi_progress =
      [on_progress, evidence_observation_total](std::size_t current,
                                                std::size_t /*total*/) {
        if (*evidence_observation_total == 0) {
          return;
        }
        on_progress("ocr_evidence_crops",
                    *evidence_observation_total + current,
                    *evidence_observation_total * 2,
                    "verifying evidence crops");
      };
}

svp::vision::DepthGenerationResult generate_depth(
    const VisionLaneSettings& settings,
    const svp::vision::DecodedCanonicalFrames& frames,
    const SpatialProgressCallback& on_progress) {
  const RasterSize raster = canonical_raster(settings.media_plan_json);
  svp::vision::DepthGenerationOptions depth_opts;
  depth_opts.model_cache_root = settings.model_cache_root;
  depth_opts.threads = settings.thread_plan.depth;
  depth_opts.raster_width = raster.width;
  depth_opts.raster_height = raster.height;
  depth_opts.frame_input = frames;
  if (on_progress) {
    depth_opts.on_progress = [&on_progress](std::size_t current, std::size_t total) {
      on_progress("depth", current, total, "");
    };
  }

  svp::vision::DepthGenerationResult result;
  try {
    result = svp::vision::generate_depth_blocks(depth_opts, settings.staging_dir);
  } catch (const std::exception& e) {
    result.blocker = std::string("Depth generation error: ") + e.what();
    result.onnx_runtime_available = settings.model_runtime_available;
  }
  return result;
}

}  // namespace

svp::vision::DecodedCanonicalFrames decode_vision_lane_canonical_frames(
    const VisionLaneSettings& settings, svp::vision::FrameCatalog* frame_catalog) {
  if (settings.media_plan == nullptr || settings.ffmpeg_path.empty()) {
    return {};
  }
  return svp::vision::decode_canonical_frames(*settings.media_plan,
                                              settings.ffmpeg_path, frame_catalog);
}

VisionDepthStageResult run_vision_depth_stage(
    const VisionLaneSettings& settings,
    const svp::vision::DecodedCanonicalFrames& frames,
    const SpatialProgressCallback& on_progress) {
  VisionDepthStageResult result;
  if (!settings.model_runtime_available) {
    detail::write_depth_placeholder_files(settings.staging_dir);
    result.depth_index_written = true;
    result.depth_placeholder_written = true;
    result.processor = detail::make_spatial_placeholder_processor();
    return result;
  }

  svp::vision::DepthGenerationResult depth = generate_depth(settings, frames, on_progress);
  result.depth_index_written = depth.depth_index_written;
  result.depth_blocks_written = depth.depth_blocks_written;
  result.depth_generation_run = depth.depth_generation_run;
  result.depth_model_available = depth.depth_model_available;
  result.depth_model_verified = depth.depth_model_verified;
  // Frame input availability comes from the decoded frames directly, not
  // from the depth result, which may have returned early at model gating.
  result.depth_frame_input_available =
      frames.decoding_succeeded && !frames.frames.empty();
  result.depth_generation_detail = svp::vision::depth_generation_result_to_json(depth);

  if (!depth.depth_blocks_written) {
    // depth_blocks_written stays false: it means real SVPB depth blocks were
    // generated. The placeholder is an honest empty file.
    detail::write_depth_placeholder_files(settings.staging_dir);
    result.depth_index_written = true;
    result.depth_placeholder_written = true;
  }
  result.processor = depth.processor_provenance.is_object()
                         ? depth.processor_provenance
                         : detail::make_spatial_placeholder_processor();
  return result;
}

svp::vision::OcrGenerationOptions make_vision_ocr_options(
    const VisionLaneSettings& settings,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress) {
  const RasterSize raster = canonical_raster(settings.media_plan_json);

  // OCR decodes its own higher-resolution frames for text detection; the
  // canonical frames are its fallback input.
  svp::vision::OcrGenerationOptions ocr_opts;
  ocr_opts.model_cache_root = settings.model_cache_root;
  ocr_opts.ffmpeg_path = settings.ffmpeg_path;
  ocr_opts.media_plan = settings.media_plan;
  ocr_opts.canonical_raster_width = static_cast<int>(raster.width);
  ocr_opts.canonical_raster_height = static_cast<int>(raster.height);
  if (settings.media_plan != nullptr) {
    const auto ocr_dims =
        svp::vision::ocr_decode_frame_dimensions(*settings.media_plan);
    ocr_opts.ocr_frame_width = ocr_dims.width;
    ocr_opts.ocr_frame_height = ocr_dims.height;
  }
  if (settings.model_runtime_available) {
    // Evidence crops are bounded crop images kept as visual evidence.
    ocr_opts.generate_evidence_crops = (settings.media_plan != nullptr);
    ocr_opts.crop_coverage_policy = kEvidenceCropCoveragePolicy;
    ocr_opts.crop_min_jpeg_quality = kEvidenceCropMinJpegQuality;
  }
  ocr_opts.performance_profile = settings.performance.ocr_performance_profile;
  ocr_opts.recognition_parallel_workers =
      settings.thread_plan.ocr_recognition_workers;
  ocr_opts.detection_threads = settings.thread_plan.ocr_detection;
  ocr_opts.recognition_threads = settings.thread_plan.ocr_recognition;
  ocr_opts.frame_catalog = frame_catalog;
  attach_ocr_progress_callbacks(ocr_opts, on_progress);
  return ocr_opts;
}

namespace {

VisionOcrStageResult vision_ocr_stage_result(const svp::vision::OcrGenerationResult& ocr) {
  VisionOcrStageResult result;
  result.ocr_available = ocr.ocr_available;
  result.ocr_frame_input_available = ocr.ocr_frame_input_available;
  result.ocr_detection_run = ocr.ocr_detection_run;
  result.ocr_recognition_run = ocr.ocr_recognition_run;
  result.text_observation_count = static_cast<std::size_t>(ocr.text_observation_count);
  result.numeric_value_count = static_cast<std::size_t>(ocr.numeric_value_count);
  result.ocr_generation_detail = svp::vision::ocr_generation_result_to_json(ocr);
  result.processors = ocr.processors;
  return result;
}

}  // namespace

VisionOcrStageResult run_vision_ocr_stage(
    const VisionLaneSettings& settings,
    const svp::vision::DecodedCanonicalFrames& frames,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress) {
  const svp::vision::OcrGenerationOptions ocr_opts =
      make_vision_ocr_options(settings, frame_catalog, on_progress);
  svp::vision::OcrGenerationResult ocr;
  try {
    ocr = svp::vision::generate_ocr_observations(ocr_opts, frames,
                                                 settings.staging_dir);
  } catch (const std::exception& e) {
    ocr.blocker = std::string("OCR generation error: ") + e.what();
  }
  return vision_ocr_stage_result(ocr);
}

VisionOcrStageResult run_vision_ocr_reduce_stage(
    const VisionLaneSettings& settings,
    const svp::vision::OcrSamplePlan& plan,
    std::vector<std::vector<svp::vision::OcrSampleDetections>> batch_results,
    const svp::vision::PpOcrSession& pp_ocr_session,
    const svp::vision::PpOcrOptions& pp_ocr_options,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress) {
  const svp::vision::OcrGenerationOptions ocr_opts =
      make_vision_ocr_options(settings, frame_catalog, on_progress);
  svp::vision::OcrGenerationResult ocr;
  try {
    ocr = svp::vision::reduce_ocr_frame_batches(ocr_opts, plan, std::move(batch_results),
                                                pp_ocr_session, pp_ocr_options,
                                                settings.staging_dir);
  } catch (const svp::vision::OcrFrameBatchReductionError&) {
    // Batches that do not cover the plan are a build failure, never an OCR
    // stage that silently produced nothing.
    throw;
  } catch (const std::exception& e) {
    ocr.blocker = std::string("OCR generation error: ") + e.what();
  }
  return vision_ocr_stage_result(ocr);
}

VisionTextEmbeddingStageResult run_vision_text_embedding_stage(
    const VisionLaneSettings& settings,
    const SpatialProgressCallback& on_progress) {
  VisionTextEmbeddingStageResult result;
  svp::vision::EmbeddingGenerationResult embeddings;
  if (settings.model_runtime_available) {
    svp::vision::EmbeddingGenerationOptions emb_opts;
    emb_opts.model_cache_root = settings.model_cache_root;
    emb_opts.threads = settings.thread_plan.text_embedding;
    emb_opts.media_plan = settings.media_plan;
    emb_opts.ffmpeg_path = settings.ffmpeg_path;
    emb_opts.vision_threads = settings.thread_plan.visual_entity_embedding;
    if (on_progress) {
      emb_opts.on_progress = [&on_progress](std::size_t current, std::size_t total) {
        on_progress("text_embeddings", current, total, "");
      };
    }
    try {
      embeddings = svp::vision::generate_embedding_blocks(emb_opts, settings.staging_dir);
    } catch (const std::exception& e) {
      embeddings.blocker = std::string("Embedding generation error: ") + e.what();
      embeddings.onnx_runtime_available = settings.model_runtime_available;
    }
    result.embedding_sets_written = embeddings.embedding_sets_written;
    result.embeddings_index_written = embeddings.embeddings_index_written;
    result.embeddings_blocks_written = embeddings.embeddings_blocks_written;
    result.embedding_generation_run = embeddings.embedding_generation_run;
    result.embedding_model_available = embeddings.embedding_model_available;
    result.embedding_generation_detail =
        svp::vision::embedding_generation_result_to_json(embeddings);
  }

  if (!embeddings.embeddings_blocks_written) {
    detail::write_embedding_placeholder_files(settings.staging_dir);
    result.embedding_sets_written = true;
    result.embeddings_index_written = true;
    result.embeddings_blocks_written = true;
  }
  result.processor = embeddings.processor_provenance.is_object()
                         ? embeddings.processor_provenance
                         : detail::make_embedding_placeholder_processor();
  return result;
}

VisionTrackingStageResult run_vision_tracking_stage(
    const VisionLaneSettings& settings,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress) {
  VisionTrackingStageResult result;
  // Visual tracking owns its temporal coverage independently from the
  // five-frame foundation input shared by depth and embedding generation. It
  // processes bounded overlapping decode windows for dense temporal coverage
  // and cross-window identity handoff.
  if (settings.model_runtime_available && settings.media_plan != nullptr &&
      !settings.ffmpeg_path.empty()) {
    std::vector<std::pair<std::string, std::int64_t>> shot_boundaries;
    for (const auto& shot : detail::read_jsonl_records(
             settings.staging_dir / "timeline" / "shots.jsonl")) {
      if (shot.contains("id") && shot.contains("start_us")) {
        shot_boundaries.emplace_back(shot["id"].get<std::string>(),
                                     shot["start_us"].get<std::int64_t>());
      }
    }

    svp::vision::VisualEntityPipelineOptions entity_options;
    entity_options.execution_provider = "cpu";
    entity_options.detector.threads = settings.thread_plan.visual_entity_detection;
    entity_options.depth_threads = settings.thread_plan.depth;
    entity_options.embedding_threads = settings.thread_plan.visual_entity_embedding;
    const auto parsed_quality =
        svp::vision::parse_visual_tracking_quality(settings.visual_tracking_quality);
    if (!parsed_quality) {
      throw std::invalid_argument(
          "visual tracking quality must be off, low, medium, or high");
    }
    entity_options.quality = *parsed_quality;
    if (svp::vision::visual_tracking_enabled(entity_options.quality)) {
      VisualEntityArtifactWriter artifact_writer(settings.staging_dir);
      entity_options.assembly.handoff_retention_us =
          svp::vision::visual_tracking_quality_policy(entity_options.quality)
              .window_overlap_us;
      entity_options.assembly.artifact_sink =
          [&artifact_writer](const std::vector<svp::vision::TrackedRegion>& regions,
                             const std::vector<svp::vision::MaskWriteEntry>& masks) {
            artifact_writer.append(regions, masks);
          };
      if (on_progress) {
        entity_options.on_progress = [&on_progress](std::size_t current,
                                                    std::size_t total) {
          on_progress("visual_tracking", current, total, "");
        };
      }

      auto entity_result = svp::vision::run_visual_entity_pipeline(
          *settings.media_plan, settings.ffmpeg_path, settings.model_cache_root,
          shot_boundaries, frame_catalog, entity_options);

      std::set<std::string> retained_entity_ids;
      for (const auto& entity : entity_result.assembled.tracker_result.entities) {
        retained_entity_ids.insert(entity.entity_id);
      }
      const auto streamed_artifacts = artifact_writer.finish(retained_entity_ids);

      const auto visual_entity_summary = write_visual_entity_artifacts(
          settings.staging_dir, entity_result.assembled.tracker_result, nullptr,
          &streamed_artifacts);
      result.masks_index_written = visual_entity_summary.masks_written;
      result.masks_blocks_written = visual_entity_summary.masks_written;
      result.processor = visual_entity_summary.processor_record;
    }
  }

  if (!result.masks_index_written) {
    detail::write_mask_placeholder_files(settings.staging_dir);
    result.masks_index_written = true;
    result.masks_blocks_written = true;
  }
  return result;
}

VisionLaneOutcome combine_vision_lane_results(
    bool model_runtime_available,
    const VisionDepthStageResult& depth,
    const VisionOcrStageResult& ocr,
    const VisionTextEmbeddingStageResult& embeddings,
    const VisionTrackingStageResult& tracking) {
  VisionLaneOutcome outcome;
  SpatialEmbeddingPlaceholderSummary& summary = outcome.summary;
  summary.model_runtime_available = model_runtime_available;

  summary.ocr_available = ocr.ocr_available;
  summary.ocr_frame_input_available = ocr.ocr_frame_input_available;
  summary.ocr_detection_run = ocr.ocr_detection_run;
  summary.ocr_recognition_run = ocr.ocr_recognition_run;
  summary.text_observation_count = ocr.text_observation_count;
  summary.numeric_value_count = ocr.numeric_value_count;
  summary.ocr_generation_detail = ocr.ocr_generation_detail;

  summary.depth_index_written = depth.depth_index_written;
  summary.depth_blocks_written = depth.depth_blocks_written;
  summary.depth_placeholder_written = depth.depth_placeholder_written;
  summary.depth_generation_run = depth.depth_generation_run;
  summary.depth_model_available = depth.depth_model_available;
  summary.depth_model_verified = depth.depth_model_verified;
  summary.depth_frame_input_available = depth.depth_frame_input_available;
  summary.depth_generation_detail = depth.depth_generation_detail;

  summary.embedding_sets_written = embeddings.embedding_sets_written;
  summary.embeddings_index_written = embeddings.embeddings_index_written;
  summary.embeddings_blocks_written = embeddings.embeddings_blocks_written;
  summary.embedding_generation_run = embeddings.embedding_generation_run;
  summary.embedding_model_available = embeddings.embedding_model_available;
  summary.embedding_generation_detail = embeddings.embedding_generation_detail;

  summary.masks_index_written = tracking.masks_index_written;
  summary.masks_blocks_written = tracking.masks_blocks_written;

  outcome.processor_records = ocr.processors;
  const std::vector<nlohmann::json> lane_processors{depth.processor,
                                                    embeddings.processor};
  outcome.processor_records.insert(outcome.processor_records.end(),
                                   lane_processors.begin(), lane_processors.end());
  summary.provenance_records_added += lane_processors.size();
  if (tracking.processor.is_object()) {
    outcome.processor_records.push_back(tracking.processor);
  }
  return outcome;
}

}  // namespace svp::package
