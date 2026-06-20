// Tests for the real_frame_color_sampling boundary.
//
// Covers:
// 1. no-ffmpeg path: real_decoding_attempted=false, execution_state=
//    ffmpeg_unavailable_synthetic_fallback, valid fallback.
// 2. zero-duration path: same no-attempt contract.
// 3. Honesty flags through build_real_frame_color_staging_artifact (no-ffmpeg).
// 4. Manifest canonical raster and source_path fields.
// 5. Synthetic staging regression guard.
// 6. Frame count fields (frames_attempted/decoded/missed) when ffmpeg absent.
// 7. execution_state is never the old ambiguous string.

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

// Minimal probe: 1920x1080, 30 fps, 30-second duration.
svp::media::MediaProbe make_probe_1080p_30s() {
  svp::media::VideoStreamProbe vs;
  vs.id = "vstream_0001";
  vs.index = 0;
  vs.codec_name = "h264";
  vs.width = 1920;
  vs.height = 1080;
  vs.rotation_degrees = 0;
  vs.pixel_aspect_ratio = {1, 1};
  vs.timing.timebase = {1, 90000};
  vs.timing.start_pts = 0;
  vs.timing.duration_pts = 2700000;  // 30 s at 1/90000
  vs.timing.average_frame_rate = svp::media::Rational{30, 1};
  vs.timing.frame_count = 900;

  svp::media::MediaProbe probe;
  probe.format_name = "mov,mp4,m4a,3gp,3g2,mj2";
  probe.video_streams.push_back(std::move(vs));
  return probe;
}

svp::media::MediaIngestPlan make_plan_1080p_30s() {
  return svp::media::build_media_ingest_plan(
      std::filesystem::path("/nonexistent/sample.mp4"),
      make_probe_1080p_30s());
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  // Test 1: no-ffmpeg path
  // -------------------------------------------------------------------------
  {
    const svp::media::MediaIngestPlan plan = make_plan_1080p_30s();

    const svp::vision::RealFrameSamplingResult result =
        svp::vision::build_real_frame_color_sampling_input(
            plan, std::filesystem::path("/nonexistent-ffmpeg-binary"));

    require(!result.real_decoding_attempted,
            "no-ffmpeg: real_decoding_attempted must be false");
    require(!result.real_decoding_succeeded,
            "no-ffmpeg: real_decoding_succeeded must be false");
    require(result.frames_attempted == 0,
            "no-ffmpeg: frames_attempted must be 0");
    require(result.frames_decoded == 0,
            "no-ffmpeg: frames_decoded must be 0");
    require(result.frames_missed == 0,
            "no-ffmpeg: frames_missed must be 0 (no attempt was made)");
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
  // Test 2: zero-duration probe — no attempt made
  // -------------------------------------------------------------------------
  {
    svp::media::VideoStreamProbe vs;
    vs.id = "vstream_0001";
    vs.index = 0;
    vs.width = 640;
    vs.height = 360;
    vs.pixel_aspect_ratio = {1, 1};
    vs.timing.timebase = {0, 1};  // invalid → duration stays 0

    svp::media::MediaProbe probe;
    probe.format_name = "mp4";
    probe.video_streams.push_back(std::move(vs));

    const svp::media::MediaIngestPlan plan =
        svp::media::build_media_ingest_plan(
            std::filesystem::path("/nonexistent/noduration.mp4"),
            std::move(probe));

    // Use the "ffmpeg" name which may or may not be present, but duration
    // check happens first so decoding_attempted should still be false.
    const svp::vision::RealFrameSamplingResult result =
        svp::vision::build_real_frame_color_sampling_input(
            plan, std::filesystem::path("ffmpeg"));

    require(!result.real_decoding_attempted,
            "zero-duration: real_decoding_attempted must be false");
    require(!result.real_decoding_succeeded,
            "zero-duration: decoding must not succeed");
    require(result.frames_attempted == 0,
            "zero-duration: frames_attempted must be 0");
    require(!result.input.frames.empty(),
            "zero-duration: fallback must contain frames");
  }

  // -------------------------------------------------------------------------
  // Test 3: execution_state is correct for no-ffmpeg path
  // -------------------------------------------------------------------------
  {
    const svp::media::MediaIngestPlan plan = make_plan_1080p_30s();

    const svp::vision::FoundationColorStagingArtifact artifact =
        svp::vision::build_real_frame_color_staging_artifact(
            plan, std::filesystem::path("/nonexistent-ffmpeg-binary"));

    const nlohmann::json json =
        svp::vision::foundation_color_staging_artifact_to_json(artifact);

    const std::string state =
        json.at("manifest").at("execution_state").get<std::string>();

    require(state == "ffmpeg_unavailable_synthetic_fallback",
            "no-ffmpeg: execution_state must be ffmpeg_unavailable_synthetic_fallback, got: " + state);

    // The old ambiguous string must never appear.
    require(state != "real_frame_decoding_attempted_fallback_to_synthetic",
            "no-ffmpeg: old ambiguous execution_state must not appear");

    require(json.at("manifest").at("real_media_frame_decoding_run") == false,
            "no-ffmpeg artifact: real_media_frame_decoding_run must be false");
    require(json.at("manifest").at("real_decoding_attempted") == false,
            "no-ffmpeg artifact: real_decoding_attempted must be false");
    require(json.at("manifest").at("package_writer_run") == false,
            "no-ffmpeg artifact: package_writer_run must be false");
    require(json.at("manifest").at("valid_svp_package_written") == false,
            "no-ffmpeg artifact: valid_svp_package_written must be false");
    require(!json.at("colors").at("color_observations").empty(),
            "no-ffmpeg artifact: must have color observations");
    require(json.at("colors").at("color_absence").at("color_completed") == true,
            "no-ffmpeg artifact: color_absence must show completed");
  }

  // -------------------------------------------------------------------------
  // Test 4: manifest canonical raster and source_path fields
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
    require(json.at("manifest").at("frames_attempted") == 0,
            "no-ffmpeg: frames_attempted in manifest must be 0");
    require(json.at("manifest").at("frames_decoded") == 0,
            "no-ffmpeg: frames_decoded in manifest must be 0");
    require(json.at("manifest").at("frames_missed") == 0,
            "no-ffmpeg: frames_missed in manifest must be 0");
  }

  // -------------------------------------------------------------------------
  // Test 5: existing synthetic staging path is unchanged (regression guard)
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

  // -------------------------------------------------------------------------
  // Test 6: partial decode result shape (simulated via a plan that has
  //         duration but no real file — decoding will miss all frames when
  //         ffmpeg is present but the path doesn't exist, or succeed if real
  //         media is present).  We test the struct contract, not decode output.
  // -------------------------------------------------------------------------
  {
    // If ffmpeg exists in PATH but the source file doesn't, decoding_attempted
    // should be true, decoding_succeeded should be false, and
    // execution_state should be real_frame_decoding_all_missed_synthetic_fallback.
    const svp::media::MediaIngestPlan plan = make_plan_1080p_30s();
    const std::filesystem::path ffmpeg_path("ffmpeg");

    const svp::vision::RealFrameSamplingResult result =
        svp::vision::build_real_frame_color_sampling_input(plan, ffmpeg_path);

    // frames_decoded + frames_missed == frames_attempted when attempt was made.
    if (result.real_decoding_attempted) {
      require(result.frames_decoded + result.frames_missed == result.frames_attempted,
              "partial-decode: frames_decoded + frames_missed must equal frames_attempted");
    }

    // Regardless of outcome, the input must be valid (non-empty frames).
    require(!result.input.frames.empty(),
            "partial-decode: input frames must be non-empty regardless of decode outcome");
    require(!result.input.scenes.empty(),
            "partial-decode: input scenes must be non-empty");
    require(!result.input.shots.empty(),
            "partial-decode: input shots must be non-empty");

    // execution_state must be one of the four valid values.
    const svp::vision::FoundationColorStagingArtifact artifact =
        svp::vision::build_real_frame_color_staging_artifact(plan, ffmpeg_path);
    const nlohmann::json json =
        svp::vision::foundation_color_staging_artifact_to_json(artifact);
    const std::string state =
        json.at("manifest").at("execution_state").get<std::string>();

    const bool valid_state =
        state == "ffmpeg_unavailable_synthetic_fallback" ||
        state == "real_frame_decoding_all_missed_synthetic_fallback" ||
        state == "real_frame_decoding_partial" ||
        state == "real_frame_decoding_succeeded";
    require(valid_state,
            "partial-decode: execution_state must be one of the four valid values, got: " + state);

    // The old ambiguous value must never appear.
    require(state != "real_frame_decoding_attempted_fallback_to_synthetic",
            "partial-decode: old ambiguous execution_state must never appear");
  }

  std::cout << "All real_frame_color_sampling tests passed.\n";
  return 0;
}
