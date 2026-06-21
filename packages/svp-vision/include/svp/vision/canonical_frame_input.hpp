#pragma once

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/color_frame_sampling.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::vision {

// Result of decoding real canonical RGB frames from source media.
//
// This is the shared frame-decoding module used by both the color pipeline
// and the depth generation pipeline.  It owns the decoded pixel buffers so
// callers can feed real frame data to downstream processors (color
// quantization, ONNX depth inference, etc.) without duplicating the
// ffmpeg-based decoding logic.
//
// decoding_attempted  - true when ffmpeg was found and pre-conditions were
//   met (duration known, canonical raster positive).  False means no decode
//   was tried at all.
//
// decoding_succeeded  - true when at least one frame was successfully
//   decoded from the source.  Always false when decoding_attempted is false.
//
// frames_attempted  - how many timestamps were presented to ffmpeg
// frames_decoded    - how many of those produced a full pixel buffer
// frames_missed     - frames_attempted - frames_decoded
struct DecodedCanonicalFrames {
  std::vector<ColorRasterFrame> frames;
  bool decoding_attempted = false;
  bool decoding_succeeded = false;
  int frames_attempted = 0;
  int frames_decoded = 0;
  int frames_missed = 0;
  std::string skipped_reason;
};

// Decode real canonical RGB frames from the source media at deterministic
// timestamps using ffmpeg.  Frames are scaled to the canonical analysis
// raster computed from the MediaIngestPlan.
//
// Individual per-frame decode misses are tolerated: if some timestamps fail
// but at least one frame is decoded the result contains only the
// successfully decoded frames.  decoding_succeeded and the
// frames_decoded/missed counters reflect the actual outcome.
//
// If ffmpeg is not available, if the duration is unknown, or if every
// targeted frame fails, the result is empty with decoding_attempted and
// decoding_succeeded set accordingly.
[[nodiscard]] DecodedCanonicalFrames decode_canonical_frames(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path);

// Decode frames at a custom resolution using the same deterministic timestamp
// selection as decode_canonical_frames.  Used by OCR generation which needs
// higher-resolution frames than the canonical analysis raster to detect text.
// Bounding boxes from OCR at this resolution can be normalized to canonical
// raster coordinates by the caller.
[[nodiscard]] DecodedCanonicalFrames decode_frames_at_resolution(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    int target_width,
    int target_height,
    int max_frames);

// Extract the media duration in microseconds from the MediaIngestPlan.
// Returns 0 if the duration cannot be determined.
[[nodiscard]] std::int64_t compute_media_duration_us(
    const media::MediaIngestPlan& plan);

// Decode frames at a custom resolution at explicit timestamps.
// Used by OCR temporal sampling which computes its own timestamp list.
[[nodiscard]] DecodedCanonicalFrames decode_frames_at_timestamps(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    int target_width,
    int target_height,
    const std::vector<std::int64_t>& timestamps_us);

}  // namespace svp::vision
