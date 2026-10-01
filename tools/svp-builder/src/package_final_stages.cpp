#include "package_final_stages.hpp"
#include "processor_provenance.hpp"

#include "svp/package/entity_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"
#include "svp/package/timeline_writer.hpp"
#include "svp/package/validation_report_storage.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/validation/validator.hpp"

#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>

namespace svp::builder {

nlohmann::json make_package_manifest(const BuildPipelineOptions& options,
                                     const nlohmann::json& canonical_analysis_raster) {
  std::time_t now = std::time(nullptr);
  char buf[100];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
  std::string created_utc(buf);

  return {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_" +
                         std::filesystem::path(options.source_path).stem().string() +
                         "_pkg"},
      {"created_utc", created_utc},
      {"primary_media_id", "media_000001"},
      {"timebase", {
        {"unit", "microseconds"},
        {"origin", "primary_presentation_start"},
        {"source_timebase_mode", "exact_rational"},
        {"rounding", "round_half_to_even"}
      }},
      {"canonical_analysis_raster", canonical_analysis_raster},
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
}

PackageEntitiesStageResult run_package_entities_stage(
    BuildPipelineContext& context,
    const std::vector<nlohmann::json>& vision_processor_records) {
  PackageEntitiesStageResult result;
  // Every lane has finished: compose processors.jsonl from each stage's
  // records, independent of the order the lanes finished in.
  compose_processor_provenance(context.staging_dir, vision_processor_records);

  // Rewrite frames.jsonl with the complete frame catalog so that every frame
  // ID referenced by OCR, depth, masks, entities, and relationships is present
  // in the timeline.
  result.frame_catalog_total_frames = svp::package::rewrite_frames_jsonl(
      context.staging_dir, context.plan, context.frame_catalog);

  // Entities come after OCR text regions and observations exist, so they
  // have real evidence.
  emit_stage_started(context, ProgressStageId::entities);
  const svp::package::EntityWriteSummary entity_summary =
      svp::package::write_entity_artifacts(context.staging_dir);
  emit_stage_completed(context, ProgressStageId::entities);
  result.entity_artifacts = svp::package::entity_write_summary_to_json(entity_summary);
  return result;
}

nlohmann::json run_package_relationships_stage(BuildPipelineContext& context) {
  // Relationships and provenance come after every source artifact exists, so
  // the relationship graph has no dangling references.
  emit_stage_started(context, ProgressStageId::relationships);
  const svp::package::RelationshipProvenanceWriteSummary relationship_summary =
      svp::package::write_relationships_and_provenance(context.staging_dir);
  emit_stage_completed(context, ProgressStageId::relationships);
  return svp::package::relationship_provenance_write_summary_to_json(
      relationship_summary);
}

void run_package_index_stage(BuildPipelineContext& context,
                             const nlohmann::json& manifest) {
  emit_stage_started(context, ProgressStageId::index);
  nlohmann::json manifest_json = manifest;
  if (!svp::package::write_index_foundation(context.staging_dir, manifest_json)) {
    std::cerr << "Warning: failed to write SQLite index foundation.\n";
    emit_warning(context, ProgressStageId::index,
                 "Failed to write SQLite index foundation.");
  }
  emit_stage_completed(context, ProgressStageId::index);
}

PackageSkeletonStageResult run_package_validation_stage(
    BuildPipelineContext& context, const nlohmann::json& manifest) {
  PackageSkeletonStageResult result;
  const BuildOutputPaths output_paths =
      resolve_package_skeleton_output_paths(context.options.output_path);
  result.package_path = output_paths.package_path;
  result.json_output_path = output_paths.json_output_path;

  // First package write, without a validation report.
  emit_stage_started(context, ProgressStageId::package_write);
  result.package_written = svp::package::write_package_skeleton(
      result.package_path, context.staging_dir, context.options.source_path,
      manifest);
  if (!result.package_written) {
    emit_stage_failed(context, ProgressStageId::package_write);
    return result;
  }
  emit_stage_completed(context, ProgressStageId::package_write);

  emit_stage_started(context, ProgressStageId::validate);
  result.validation_report_json = svp::validation::validate_package(
      result.package_path, svp::validation::ValidatorOptions{});
  emit_stage_completed(context, ProgressStageId::validate);

  // Store the report in staging for the final package write.
  emit_stage_started(context, ProgressStageId::validation_report);
  result.validation_report_stored = svp::package::write_validation_report_to_staging(
      context.staging_dir, result.validation_report_json);
  emit_stage_completed(context, ProgressStageId::validation_report);
  return result;
}

PackageSkeletonStageResult run_package_write_stage(
    BuildPipelineContext& context, const nlohmann::json& manifest,
    const PackageSkeletonStageResult& validation) {
  PackageSkeletonStageResult result = validation;
  if (!result.package_written) {
    return result;
  }
  if (!result.validation_report_stored) {
    result.package_written = false;
    return result;
  }

  // Re-package with the validation report included.
  emit_stage_started(context, ProgressStageId::repackage);
  result.package_written = svp::package::write_package_skeleton(
      result.package_path, context.staging_dir, context.options.source_path,
      manifest);
  emit_stage_completed(context, ProgressStageId::repackage);

  if (result.package_written) {
    emit_stage_started(context, ProgressStageId::validate);
    auto final_report = svp::validation::validate_package(
        result.package_path, svp::validation::ValidatorOptions{});
    result.validator_exit_code = svp::validation::exit_code(final_report);
    result.validator_passes = (result.validator_exit_code == 0);
    result.validation_report_json = final_report;
    emit_stage_completed(context, ProgressStageId::validate);
  }
  return result;
}

nlohmann::json package_stage_result_to_json(const PackageSkeletonStageResult& result) {
  return {{"package_written", result.package_written},
          {"validator_passes", result.validator_passes},
          {"validation_report_stored", result.validation_report_stored},
          {"validator_exit_code", result.validator_exit_code},
          {"validation_report", result.validation_report_json}};
}

PackageSkeletonStageResult package_stage_result_from_json(
    const nlohmann::json& value, const BuildPipelineOptions& options) {
  PackageSkeletonStageResult result;
  result.package_written = value.at("package_written").get<bool>();
  result.validator_passes = value.at("validator_passes").get<bool>();
  result.validation_report_stored = value.at("validation_report_stored").get<bool>();
  result.validator_exit_code = value.at("validator_exit_code").get<int>();
  result.validation_report_json = value.at("validation_report");
  const BuildOutputPaths output_paths =
      resolve_package_skeleton_output_paths(options.output_path);
  result.package_path = output_paths.package_path;
  result.json_output_path = output_paths.json_output_path;
  return result;
}

}  // namespace svp::builder
