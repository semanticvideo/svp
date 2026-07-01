#include "svp/vision/real_frame_color_sampling.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/foundation_color_staging.hpp"
#include "svp/vision/scene_segmentation.hpp"

namespace svp::vision {

// Maximum number of canonical frames to decode for the color sampling path.
// More frames than the default decode_canonical_frames() count (5) are used
// here so that color-change-based scene segmentation has enough temporal
// resolution to detect boundaries in a 30-second video.
constexpr int kColorMaxDecodedFrames = 15;

RealFrameSamplingResult build_real_frame_color_sampling_input(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    FrameCatalog* frame_catalog,
    FrameProgressCallback on_progress) {
  RealFrameSamplingResult result;

  const DecodedCanonicalFrames decoded =
      decode_frames_at_resolution(
          plan, ffmpeg_path,
          plan.canonical_raster.width,
          plan.canonical_raster.height,
          kColorMaxDecodedFrames,
          frame_catalog,
          "color",
          on_progress);

  result.real_decoding_attempted = decoded.decoding_attempted;
  result.real_decoding_succeeded = decoded.decoding_succeeded;
  result.frames_attempted = decoded.frames_attempted;
  result.frames_decoded = decoded.frames_decoded;
  result.frames_missed = decoded.frames_missed;
  result.skipped_reason = decoded.skipped_reason;

  if (!decoded.decoding_succeeded) {
    result.input = build_foundation_color_staging_sample_input();
    return result;
  }

  // Segment decoded frames into scenes and shots using deterministic
  // dominant-bucket-change detection.  min_scene_frames=1 allows
  // single-frame scenes so that short color-block segments (e.g. a 2-second
  // pink/green/blue block in a 30-second video) are not merged away.
  const SceneSegmentationResult segmentation =
      segment_frames_by_color_change(decoded.frames, 1);

  // Convert SceneSegment objects to ColorTimelineRange for the color pipeline.
  std::vector<ColorTimelineRange> scene_ranges;
  scene_ranges.reserve(segmentation.scenes.size());
  for (const SceneSegment& seg : segmentation.scenes) {
    scene_ranges.push_back(ColorTimelineRange{
        seg.scene_id,
        seg.start_us,
        seg.end_us,
        seg.frame_ids,
    });
  }

  result.real_decoding_succeeded = true;
  result.input = ColorFrameSamplingInput{
      std::move(decoded.frames),
      std::move(scene_ranges),
      std::move(segmentation.shots),
  };
  return result;
}

}  // namespace svp::vision
