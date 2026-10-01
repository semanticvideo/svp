#pragma once

// The package lane after audio and vision join, one function per stage
// (formerly one run_package_final_stage). In build order:
//   entities         processor merge, frames.jsonl rewrite, entities/
//   relationships    relationships/ and provenance/
//   index            index/
//   validation       draft package, validation, provenance/validation.json
//   package write    package with the stored report, final validation
// Running them in this order is exactly the former final stage.

#include "build_pipeline_internal.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>

namespace svp::builder {

// The package manifest (created_utc is stamped now, as before). Every later
// package stage writes this exact manifest.
[[nodiscard]] nlohmann::json make_package_manifest(
    const BuildPipelineOptions& options,
    const nlohmann::json& canonical_analysis_raster);

struct PackageEntitiesStageResult {
  std::size_t frame_catalog_total_frames = 0;
  nlohmann::json entity_artifacts = nlohmann::json::object();
};

// Merges the vision lane's processor records, rewrites timeline/frames.jsonl
// from `frame_catalog`, and writes entities.
[[nodiscard]] PackageEntitiesStageResult run_package_entities_stage(
    BuildPipelineContext& context,
    const std::vector<nlohmann::json>& vision_processor_records);

// Returns the relationship/provenance write summary JSON.
[[nodiscard]] nlohmann::json run_package_relationships_stage(
    BuildPipelineContext& context);

void run_package_index_stage(BuildPipelineContext& context,
                             const nlohmann::json& manifest);

// Writes the draft package, validates it, and stores the report in staging.
[[nodiscard]] PackageSkeletonStageResult run_package_validation_stage(
    BuildPipelineContext& context, const nlohmann::json& manifest);

// Re-packages with the stored report and validates the result. A failed
// draft (`validation`) is passed through unchanged.
[[nodiscard]] PackageSkeletonStageResult run_package_write_stage(
    BuildPipelineContext& context, const nlohmann::json& manifest,
    const PackageSkeletonStageResult& validation);

// Package stage results cross task boundaries as JSON. Paths are not stored:
// they follow from the build options.
[[nodiscard]] nlohmann::json package_stage_result_to_json(
    const PackageSkeletonStageResult& result);
[[nodiscard]] PackageSkeletonStageResult package_stage_result_from_json(
    const nlohmann::json& value, const BuildPipelineOptions& options);

}  // namespace svp::builder
