#include "build_pipeline_internal.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace svp::builder {

void run_foundation_ocr_stage(BuildPipelineContext& context) {
  // Decode canonical frames (used as fallback) and run real OCR.
  // OCR decodes its own higher-resolution frames for text detection.
  svp::vision::DecodedCanonicalFrames decoded_frames =
      svp::vision::decode_canonical_frames(context.plan, context.options.ffmpeg_path,
                                           &context.frame_catalog);

  svp::vision::OcrGenerationOptions ocr_opts;
  ocr_opts.model_cache_root = std::filesystem::path(context.options.model_cache_dir);
  ocr_opts.ffmpeg_path = context.options.ffmpeg_path;
  ocr_opts.media_plan = &context.plan;
  ocr_opts.canonical_raster_width = context.plan.canonical_raster.width;
  ocr_opts.canonical_raster_height = context.plan.canonical_raster.height;
  {
    int src_w = static_cast<int>(context.plan.primary_video_stream.width);
    int src_h = static_cast<int>(context.plan.primary_video_stream.height);
    if (std::abs(context.plan.primary_video_stream.rotation_degrees) == 90) {
      std::swap(src_w, src_h);
    }
    const int max_ocr_dim = 1920;
    if (src_w > max_ocr_dim || src_h > max_ocr_dim) {
      if (src_w >= src_h) {
        ocr_opts.ocr_frame_width = max_ocr_dim;
        ocr_opts.ocr_frame_height = static_cast<int>(
            std::round(static_cast<double>(src_h) * max_ocr_dim / src_w));
      } else {
        ocr_opts.ocr_frame_height = max_ocr_dim;
        ocr_opts.ocr_frame_width = static_cast<int>(
            std::round(static_cast<double>(src_w) * max_ocr_dim / src_h));
      }
    } else {
      ocr_opts.ocr_frame_width = src_w;
      ocr_opts.ocr_frame_height = src_h;
    }
  }

  ocr_opts.frame_catalog = &context.frame_catalog;
  ocr_opts.generate_evidence_crops = true;
  ocr_opts.crop_coverage_policy = "one_per_observation";
  ocr_opts.crop_min_jpeg_quality = 50;
  ocr_opts.on_progress = [&context](int current, int total) {
    if (total > 0) {
      emit_stage_progress(context, ProgressStageId::ocr,
                          static_cast<std::uint64_t>(current),
                          static_cast<std::uint64_t>(total),
                          "frames");
    }
  };
  bool evidence_crop_stage_started = false;
  std::uint64_t evidence_crop_observation_total = 0;
  std::uint64_t evidence_crop_current = 0;
  auto start_evidence_crop_stage =
      [&context, &evidence_crop_stage_started]() {
        if (!evidence_crop_stage_started) {
          emit_stage_started(context, ProgressStageId::ocr_evidence_crops);
          evidence_crop_stage_started = true;
        }
      };
  ocr_opts.on_evidence_crop_progress =
      [&context,
       &evidence_crop_observation_total,
       &evidence_crop_current,
       &start_evidence_crop_stage](std::size_t current, std::size_t total) {
        if (total == 0) {
          return;
        }
        evidence_crop_observation_total = static_cast<std::uint64_t>(total);
        evidence_crop_current = static_cast<std::uint64_t>(current);
        start_evidence_crop_stage();
        emit_stage_progress(
            context,
            ProgressStageId::ocr_evidence_crops,
            evidence_crop_current,
            evidence_crop_observation_total * 2,
            "steps",
            "extracting evidence crops");
      };
  ocr_opts.on_evidence_roi_progress =
      [&context,
       &evidence_crop_observation_total,
       &evidence_crop_current,
       &start_evidence_crop_stage](std::size_t current, std::size_t /*total*/) {
        if (evidence_crop_observation_total == 0) {
          return;
        }
        evidence_crop_current =
            evidence_crop_observation_total + static_cast<std::uint64_t>(current);
        start_evidence_crop_stage();
        emit_stage_progress(
            context,
            ProgressStageId::ocr_evidence_crops,
            evidence_crop_current,
            evidence_crop_observation_total * 2,
            "steps",
            "verifying evidence crops");
      };

  svp::vision::OcrGenerationResult ocr_result =
      svp::vision::generate_ocr_observations(
          ocr_opts, decoded_frames, context.staging_dir);
  if (evidence_crop_stage_started && ocr_result.blocker.empty()) {
    const std::uint64_t evidence_crop_total = evidence_crop_observation_total * 2;
    if (evidence_crop_current < evidence_crop_total) {
      emit_stage_progress(context,
                          ProgressStageId::ocr_evidence_crops,
                          evidence_crop_total,
                          evidence_crop_total,
                          "steps",
                          "verifying evidence crops");
    }
    emit_stage_completed(context, ProgressStageId::ocr_evidence_crops);
  }

  // Append OCR processor provenance records
  if (!ocr_result.processors.empty()) {
    nlohmann::json procs = nlohmann::json::array();
    for (const auto& p : ocr_result.processors) procs.push_back(p);
    append_jsonl_file(context.staging_dir / "provenance" / "processors.jsonl", procs);
  }

  nlohmann::json ocr_json =
      svp::vision::ocr_generation_result_to_json(ocr_result);
  ocr_json["staging_paths"] = {
      {"text_regions_jsonl",
       (context.staging_dir / "text" / "text_regions.jsonl").string()},
      {"text_observations_jsonl",
       (context.staging_dir / "text" / "text_observations.jsonl").string()},
      {"numeric_values_jsonl",
       (context.staging_dir / "text" / "numeric_values.jsonl").string()},
      {"text_absence_json",
       (context.staging_dir / "text" / "text_absence.json").string()},
      {"processors_jsonl",
       (context.staging_dir / "provenance" / "processors.jsonl").string()},
  };
  context.output["foundation_ocr_staging"] = ocr_json;
}

}  // namespace svp::builder
