// Tests for the real_frame_color_sampling boundary.
//
// These tests verify:
// 1. The fallback path (no ffmpeg / no source) produces a valid synthetic
//    input with real_decoding_attempted=false.
// 2. The decoding-attempted-but-failed path sets real_decoding_succeeded=false.
// 3. The RealFrameSamplingResult always yields a non-empty input that the
//    existing color pipeline can consume without throwing.
// 4. Honesty flags propagate correctly through build_real_frame_color_staging_artifact.

#include "svp/vision/real_frame_color_sampling.hpp"
#include "svp/vision/foundation_color_staging.hpp"
#include "svp/vision/color_frame_sampling.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/media/rational.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

// Minimal probe for a 16x9 1920x1080 video with a 30-second duration.
svp::media::MediaProbe make_probe_1080p_30s() {
  svp::media::VideoStreamProbe vs;
  vs.id = "vstream_0001";
  vs.index = 0;
  vs.codec_name = "h264";
  vs.width = 1920;
  vs.height = 1080;
  vs.rotation_degrees = 0;
  vs.pixel_aspect_ratio = {1, 1};
  // 30 s at 1/90000 timebase -> 2700000 pts
  vs.timing.timebase = {1, 90000};
  vs.timing.start_pts = 0;
  vs.timing.duration_pts = 2700000;
  vs.timing.average_frame_rate = svp::media::Rational{30, 1};
  vs.timing.frame_count = 900;

  svp::media::MediaProbe probe;
  probe.format_name = "mov,mp4,m4a,3gp,3g2,mj2";
  probe.video_streams.push_back(std::move(vs));
  return probe;
}

// Build a MediaIngestPlan from the probe above.
svp::media::MediaIngestPlan make_plan_1080p_30s() {
  return svp::media::build_media_ingest_plan(
      std::filesystem::path("/nonexistent/sample.mp4"),
      make_probe_1080p_30s());
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  // Test 1: no-ffmpeg path → real_decoding_attempted=false, valid fallback
  // -------------------------------------------------------------------------
  {
    const svp::media::MediaIngestPlan plan = make_plan_1080p_30s();

    // Use a path that cannot exist.
    const svp::vision::RealFrameSamplingResult result =
        svp::vision::build_real_frame_color_sampling_input(
            plan, std::filesystem::path("/nonexistent-ffmpeg-binary"));

    require(!result.real_decoding_attempted,
            "no-ffmpeg: real_decoding_attempted must be false");
    require(!result.real_decoding_succeeded,
            "no-ffmpeg: real_decoding_succeeded must be false");
    require(!result.skipped_reason.empty(),
            "no-ffmpeg: skipped_reason must be non-empty");
    require(!result.input.frames.empty(),
            "no-ffmpeg: fallback input must contain frames");
    require(!result.input.scenes.empty(),
            "no-ffmpeg: fallback input must contain scenes");
    require(!result.input.shots.empty(),
            "no-ffmpeg: fallback input must contain shots");
  }

  // -------------------------------------------------------------------------
  // Test 2: probe with zero duration → skipped, fallback used
  // -------------------------------------------------------------------------
  {
    svp::media::VideoStreamProbe vs;
    vs.id = "vstream_0001";
    vs.index = 0;
    vs.width = 640;
    vs.height = 360;
    vs.pixel_aspect_ratio = {1, 1};
    vs.timing.timebase = {0, 1};  // zero timebase → duration unknown

    svp::media::MediaProbe probe;
    probe.format_name = "mp4";
    probe.video_streams.push_back(std::move(vs));

    const svp::media::MediaIngestPlan plan =
        svp::media::build_media_ingest_plan(
            std::filesystem::path("/nonexistent/noduration.mp4"),
            std::move(probe));

    // Even if ffmpeg were present, duration=0 should skip.
    const svp::vision::RealFrameSamplingResult result =
        svp::vision::build_real_frame_color_sampling_input(
            plan, std::filesystem::path("ffmpeg"));

    require(!result.real_decoding_succeeded,
            "zero-duration: decoding must not succeed");
    require(!result.input.frames.empty(),
            "zero-duration: fallback must contain frames");
  }

  // -------------------------------------------------------------------------
  // Test 3: honesty flags propagate through build_real_frame_color_staging_artifact
  //         when ffmpeg is missing.
  // -------------------------------------------------------------------------
  {
    const svp::media::MediaIngestPlan plan = make_plan_1080p_30s();

    const svp::vision::FoundationColorStagingArtifact artifact =
        svp::vision::build_real_frame_color_staging_artifact(
            plan, std::filesystem::path("/nonexistent-ffmpeg-binary"));

    const nlohmann::json json =
        svp::vision::foundation_color_staging_artifact_to_json(artifact);

    require(json.at("manifest").at("real_media_frame_decoding_run") == false,
            "missing-ffmpeg artifact: real_media_frame_decoding_run must be false");
    require(json.at("manifest").at("package_writer_run") == false,
            "missing-ffmpeg artifact: package_writer_run must be false");
    require(json.at("manifest").at("valid_svp_package_written") == false,
            "missing-ffmpeg artifact: valid_svp_package_written must be false");
    require(!json.at("colors").at("color_observations").empty(),
            "missing-ffmpeg artifact: must have color observations");
    require(json.at("colors").at("color_absence").at("color_completed") == true,
            "missing-ffmpeg artifact: color_absence must show completed");
  }

  // -------------------------------------------------------------------------
  // Test 4: canonical raster dimensions from the plan survive into the manifest
  //         (available regardless of whether decoding succeeded).
  // -------------------------------------------------------------------------
  {
    const svp::media::MediaIngestPlan plan = make_plan_1080p_30s();

    const svp::vision::FoundationColorStagingArtifact artifact =
        svp::vision::build_real_frame_color_staging_artifact(
            plan, std::filesystem::path("/nonexistent-ffmpeg-binary"));

    const nlohmann::json json =
        svp::vision::foundation_color_staging_artifact_to_json(artifact);

    require(json.at("manifest").at("canonical_raster_width") == plan.canonical_raster.width,
            "manifest canonical_raster_width must match plan");
    require(json.at("manifest").at("canonical_raster_height") == plan.canonical_raster.height,
            "manifest canonical_raster_height must match plan");
    require(json.at("manifest").at("source_path") == plan.source_path.string(),
            "manifest source_path must match plan");
  }

  // -------------------------------------------------------------------------
  // Test 5: existing synthetic staging path is unchanged
  //         (regression guard).
  // -------------------------------------------------------------------------
  {
    const svp::vision::FoundationColorStagingArtifact artifact =
        svp::vision::build_foundation_color_staging_artifact();
    const nlohmann::json json =
        svp::vision::foundation_color_staging_artifact_to_json(artifact);

    require(artifact.records.records.size() == 6,
            "synthetic staging: must have 6 records");
    require(json.at("manifest").at("real_media_frame_decoding_run") == false,
            "synthetic staging: real_media_frame_decoding_run must be false");
    require(json.at("manifest").at("execution_state") ==
                "foundation_synthetic_sample_only",
            "synthetic staging: execution_state must be foundation_synthetic_sample_only");
  }

  std::cout << "All real_frame_color_sampling tests passed.\n";
  return 0;
}
