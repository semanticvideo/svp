#include "build_pipeline_internal.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/package/entity_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"
#include "svp/package/spatial_embedding_placeholders.hpp"
#include "svp/package/validation_report_storage.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/validation/validator.hpp"

#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>

namespace svp::builder {

PackageSkeletonStageResult run_package_skeleton_stage(
    BuildPipelineContext& context) {
    PackageSkeletonStageResult result;
    result.json_output_path = context.options.output_path;
    const BuildOutputPaths output_paths =
        resolve_package_skeleton_output_paths(context.options.output_path);
    result.package_path = output_paths.package_path;
    result.json_output_path = output_paths.json_output_path;

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

    // Write honest spatial/embedding placeholder entries
    // This also runs real OCR generation on decoded frames before
    // embedding generation so text_observations.jsonl is populated.
    const svp::package::SpatialEmbeddingPlaceholderSummary placeholder_summary =
        svp::package::write_spatial_and_embedding_placeholders(
            context.staging_dir, context.model_runtime_available,
            svp::media::media_ingest_plan_to_json(context.plan),
            context.options.model_cache_dir.empty()
                ? std::filesystem::path{}
                : std::filesystem::path(context.options.model_cache_dir),
            &context.plan,
            context.options.ffmpeg_path);
    context.output["spatial_embedding_placeholders"] =
        svp::package::spatial_embedding_placeholder_summary_to_json(
            placeholder_summary);

    // Write entity and entity-track artifacts after OCR text regions
    // and observations exist, so entities have real evidence.
    const svp::package::EntityWriteSummary entity_summary =
        svp::package::write_entity_artifacts(context.staging_dir);
    context.output["entity_artifacts"] =
        svp::package::entity_write_summary_to_json(entity_summary);

    // Write relationships and provenance after all source artifacts exist
    // (OCR text observations, evidence crops, depth, embeddings, entities, etc.)
    // so the relationship graph has no dangling references.
    const svp::package::RelationshipProvenanceWriteSummary relationship_summary =
        svp::package::write_relationships_and_provenance(context.staging_dir);
    context.output["package_relationships_provenance"] =
        svp::package::relationship_provenance_write_summary_to_json(
            relationship_summary);

    // Generate SQLite index foundation and manifest
    if (!svp::package::write_index_foundation(context.staging_dir, manifest_json)) {
      std::cerr << "Warning: failed to write SQLite index foundation.\n";
    }

    // First package write (without validation report)
    result.package_written =
        svp::package::write_package_skeleton(result.package_path,
                                             context.staging_dir,
                                             context.options.source_path,
                                             manifest_json);

    if (result.package_written) {
      svp::validation::ValidatorOptions validator_opts;
      validator_opts.validation_codes_path = "spec/registries/validation-codes.json";

      // Run validator on first package
      auto first_report =
          svp::validation::validate_package(result.package_path, validator_opts);
      result.validation_report_json = first_report;

      // Store validation report in staging for second package write
      result.validation_report_stored = svp::package::write_validation_report_to_staging(
          context.staging_dir, result.validation_report_json);

      if (result.validation_report_stored) {
        // Re-package with validation report included
        result.package_written =
            svp::package::write_package_skeleton(result.package_path,
                                                 context.staging_dir,
                                                 context.options.source_path,
                                                 manifest_json);
      } else {
        result.package_written = false;
      }

      if (result.package_written) {
        // Run validator on final package
        auto final_report =
            svp::validation::validate_package(result.package_path, validator_opts);
        result.validator_exit_code = svp::validation::exit_code(final_report);
        result.validator_passes = (result.validator_exit_code == 0);
        result.validation_report_json = final_report;
      }
    }
    return result;
}

}  // namespace svp::builder
