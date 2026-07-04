#include "build_pipeline_internal.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/package/entity_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"
#include "svp/package/spatial_embedding_placeholders.hpp"
#include "svp/package/timeline_writer.hpp"
#include "svp/package/validation_report_storage.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/validation/validator.hpp"

#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace svp::builder {

nlohmann::json make_package_manifest(BuildPipelineContext& context) {
    std::time_t now = std::time(nullptr);
    char buf[100];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    std::string created_utc(buf);

    nlohmann::json manifest_json = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_" +
                         std::filesystem::path(context.options.source_path)
                             .stem()
                             .string() +
                         "_pkg"},
      {"created_utc", created_utc},
      {"primary_media_id", "media_000001"},
      {"timebase", {
        {"unit", "microseconds"},
        {"origin", "primary_presentation_start"},
        {"source_timebase_mode", "exact_rational"},
        {"rounding", "round_half_to_even"}
      }},
      {"canonical_analysis_raster", context.output["canonical_analysis_raster"]},
      {"binary_block_format", {
        {"magic", "SVPB"},
        {"version", 1},
        {"header_size", 160},
        {"compression", "zstd"}
      }},
      {"hashes", {
        {"algorithm", "blake3"},
        {"digest_bytes", 32},
        {"encoding", "lowercase_hex"},
        {"manifest_excluded", true}
      }},
      {"required_sections", {
        {"media", true},
        {"transcript", true},
        {"timeline", true},
        {"entities", true},
        {"spatial", true},
        {"relationships", true},
        {"embeddings", true},
        {"index", true},
        {"provenance", true}
      }}
    };
    return manifest_json;
}

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

PackageVisionStageResult run_package_vision_stage(BuildPipelineContext& context) {
  PackageVisionStageResult result;

  // Write honest spatial/embedding placeholder entries. This also runs real OCR
  // generation on decoded frames before embedding generation so text observations
  // are available when text embeddings are generated.
  std::unordered_set<std::string> started_stages;
  std::unordered_set<std::string> completed_stages;
  std::mutex spatial_progress_mutex;
  auto spatial_progress = [&context, &started_stages, &completed_stages,
                           &spatial_progress_mutex](
                                      const char* stage,
                                      std::size_t current,
                                      std::size_t total,
                                      const char* message) {
    const std::string stage_name(stage == nullptr ? "" : stage);
    const ProgressStageId stage_id = spatial_stage_id(stage_name);
    bool should_start = false;
    bool should_complete = false;
    {
      std::lock_guard<std::mutex> lock(spatial_progress_mutex);
      should_start = started_stages.insert(stage_name).second;
      should_complete =
          current > 0 && current >= total && total > 0 &&
          completed_stages.insert(stage_name).second;
    }
    if (should_start) {
      emit_stage_started(context, stage_id);
    }
    if (total > 0) {
      emit_stage_progress(context, stage_id,
                          static_cast<std::uint64_t>(current),
                          static_cast<std::uint64_t>(total),
                          spatial_stage_unit(stage_name),
                          message == nullptr ? "" : message);
    }
    if (should_complete) {
      emit_stage_completed(context, stage_id);
    }
  };

  const svp::package::SpatialEmbeddingPlaceholderSummary placeholder_summary =
      svp::package::write_spatial_and_embedding_placeholders(
          context.staging_dir, context.model_runtime_available,
          svp::media::media_ingest_plan_to_json(context.plan),
          context.options.model_cache_dir.empty()
              ? std::filesystem::path{}
              : std::filesystem::path(context.options.model_cache_dir),
          &context.plan,
          context.options.ffmpeg_path,
          &context.frame_catalog,
          spatial_progress,
          context.options.performance,
          context.options.serial_pipeline,
          &result.processor_records);

  for (const auto& stage : {"ocr", "ocr_evidence_crops", "depth",
                            "text_embeddings", "visual_tracking",
                            "visual_embeddings"}) {
    if (started_stages.count(stage) &&
        completed_stages.insert(stage).second) {
      emit_stage_completed(context, spatial_stage_id(stage));
    }
  }
  result.placeholder_summary_json =
      svp::package::spatial_embedding_placeholder_summary_to_json(
          placeholder_summary);
  context.output["spatial_embedding_placeholders"] =
      result.placeholder_summary_json;
  return result;
}

PackageSkeletonStageResult run_package_final_stage(
    BuildPipelineContext& context,
    const PackageVisionStageResult& vision_result) {
    PackageSkeletonStageResult result;
    result.json_output_path = context.options.output_path;
    const BuildOutputPaths output_paths =
        resolve_package_skeleton_output_paths(context.options.output_path);
    result.package_path = output_paths.package_path;
    result.json_output_path = output_paths.json_output_path;

    nlohmann::json manifest_json = make_package_manifest(context);

    svp::package::merge_processor_records(
        context.staging_dir / "provenance" / "processors.jsonl",
        vision_result.processor_records);

    // Rewrite frames.jsonl with the complete frame catalog so that every
    // frame ID referenced by OCR, depth, masks, entities, and relationships
    // is present in the timeline.
    const std::size_t total_frames =
        svp::package::rewrite_frames_jsonl(context.staging_dir,
                                           context.plan,
                                           context.frame_catalog);
    context.output["frame_catalog_total_frames"] = total_frames;

    // Write entity and entity-track artifacts after OCR text regions
    // and observations exist, so entities have real evidence.
    emit_stage_started(context, ProgressStageId::entities);
    const svp::package::EntityWriteSummary entity_summary =
        svp::package::write_entity_artifacts(context.staging_dir);
    emit_stage_completed(context, ProgressStageId::entities);
    context.output["entity_artifacts"] =
        svp::package::entity_write_summary_to_json(entity_summary);

    // Write relationships and provenance after all source artifacts exist
    // (OCR text observations, evidence crops, depth, embeddings, entities, etc.)
    // so the relationship graph has no dangling references.
    emit_stage_started(context, ProgressStageId::relationships);
    const svp::package::RelationshipProvenanceWriteSummary relationship_summary =
        svp::package::write_relationships_and_provenance(context.staging_dir);
    emit_stage_completed(context, ProgressStageId::relationships);
    context.output["package_relationships_provenance"] =
        svp::package::relationship_provenance_write_summary_to_json(
            relationship_summary);

    // Generate SQLite index foundation and manifest
    emit_stage_started(context, ProgressStageId::index);
    if (!svp::package::write_index_foundation(context.staging_dir, manifest_json)) {
      std::cerr << "Warning: failed to write SQLite index foundation.\n";
      emit_warning(context, ProgressStageId::index,
                   "Failed to write SQLite index foundation.");
    }
    emit_stage_completed(context, ProgressStageId::index);

    // First package write (without validation report)
    emit_stage_started(context, ProgressStageId::package_write);
    result.package_written =
        svp::package::write_package_skeleton(result.package_path,
                                             context.staging_dir,
                                             context.options.source_path,
                                             manifest_json);

    if (!result.package_written) {
      emit_stage_failed(context, ProgressStageId::package_write);
      return result;
    }
    emit_stage_completed(context, ProgressStageId::package_write);

    svp::validation::ValidatorOptions validator_opts;
    validator_opts.validation_codes_path = "spec/registries/validation-codes.json";

    // Run validator on first package
    emit_stage_started(context, ProgressStageId::validate);
    auto first_report =
        svp::validation::validate_package(result.package_path, validator_opts);
    result.validation_report_json = first_report;
    emit_stage_completed(context, ProgressStageId::validate);

    // Store validation report in staging for second package write
    emit_stage_started(context, ProgressStageId::validation_report);
    result.validation_report_stored = svp::package::write_validation_report_to_staging(
        context.staging_dir, result.validation_report_json);
    emit_stage_completed(context, ProgressStageId::validation_report);

    if (result.validation_report_stored) {
      // Re-package with validation report included
      emit_stage_started(context, ProgressStageId::repackage);
      result.package_written =
          svp::package::write_package_skeleton(result.package_path,
                                               context.staging_dir,
                                               context.options.source_path,
                                               manifest_json);
      emit_stage_completed(context, ProgressStageId::repackage);
    } else {
      result.package_written = false;
    }

    if (result.package_written) {
      // Run validator on final package
      emit_stage_started(context, ProgressStageId::validate);
      auto final_report =
          svp::validation::validate_package(result.package_path, validator_opts);
      result.validator_exit_code = svp::validation::exit_code(final_report);
      result.validator_passes = (result.validator_exit_code == 0);
      result.validation_report_json = final_report;
      emit_stage_completed(context, ProgressStageId::validate);
    }
    return result;
}

PackageSkeletonStageResult run_package_skeleton_stage(
    BuildPipelineContext& context) {
  PackageVisionStageResult vision_result = run_package_vision_stage(context);
  return run_package_final_stage(context, vision_result);
}

}  // namespace svp::builder
