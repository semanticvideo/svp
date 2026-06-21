#pragma once

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/color_observation_records.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::vision {

struct FoundationColorStagingArtifact {
  ColorObservationRecordPlan records;
  nlohmann::json processor_provenance;
  nlohmann::json manifest;
  ColorFrameSamplingInput sampling_input;
};

// Build a staging artifact from the deterministic 2x2 synthetic sample input.
// real_media_frame_decoding_run is always false for this path.
[[nodiscard]] ColorFrameSamplingInput build_foundation_color_staging_sample_input();
[[nodiscard]] FoundationColorStagingArtifact build_foundation_color_staging_artifact();
[[nodiscard]] nlohmann::json foundation_color_staging_artifact_to_json(
    const FoundationColorStagingArtifact& artifact);

// Build a staging artifact from real decoded frames using ffmpeg.
// When real frame decoding succeeds, real_media_frame_decoding_run=true in
// the manifest.  When ffmpeg is unavailable or decoding fails, the result
// falls back to the synthetic input and real_media_frame_decoding_run=false.
[[nodiscard]] FoundationColorStagingArtifact build_real_frame_color_staging_artifact(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path);

}  // namespace svp::vision
