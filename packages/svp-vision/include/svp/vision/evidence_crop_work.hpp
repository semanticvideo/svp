#pragma once

// Per-observation work of the evidence-crop stage (dispatched_work.hpp): cut
// one crop out of the source with ffmpeg, then decode it back and re-read it
// with PP-OCR recognition (the ROI re-read). generate_evidence_crops_internal
// and generate_and_harden_evidence_crops run exactly these primitives, so a
// crop computed by an ocr.crop_batch task is the crop the stage itself would
// have written.

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

// One observation's crop, fully resolved by the stage: where to seek, the
// crop rectangle in source-frame pixels, and the encoding.
struct EvidenceCropJob {
  // Position of the observation in the stage's crop inputs.
  std::uint64_t ordinal = 0;
  std::int64_t seek_us = 0;
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
  // "jpeg" or "png".
  std::string image_format;
  int jpeg_quality = 0;

  bool operator==(const EvidenceCropJob&) const = default;
};

// The ROI re-read of an extracted crop.
struct EvidenceCropRoi {
  // False when the crop image could not be decoded back.
  bool decoded = false;
  std::string text;
  double score = 0.0;

  bool operator==(const EvidenceCropRoi&) const = default;
};

struct EvidenceCropJobOutcome {
  std::uint64_t ordinal = 0;
  // ffmpeg wrote the crop image.
  bool extracted = false;
  // The image bytes when extracted.
  std::vector<std::byte> image;
  // Set when extracted.
  EvidenceCropRoi roi;

  bool operator==(const EvidenceCropJobOutcome&) const = default;
};

// Cuts the job's crop out of `source_path` into `output_path` (ffmpeg seek,
// crop, scale toward a readable width, encode). False with `error` set when
// ffmpeg failed or wrote nothing.
bool extract_evidence_crop_image(const std::filesystem::path& ffmpeg_path,
                                 const std::filesystem::path& source_path,
                                 const EvidenceCropJob& job,
                                 const std::filesystem::path& output_path,
                                 std::string& error);

// Pixels of the image extract_evidence_crop_image writes for `job` (the crop
// after its readability scaling), for sizing what a batch of crops returns.
[[nodiscard]] std::uint64_t evidence_crop_image_pixels(const EvidenceCropJob& job);

// Decodes a crop image back to RGB at width x height (the crop's source-frame
// size), as the ROI re-read does. nullopt when the file is missing or ffmpeg
// yields no full frame.
[[nodiscard]] std::optional<ColorRasterFrame> decode_evidence_crop_image(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& image_path,
    int width,
    int height,
    const std::string& frame_id,
    std::int64_t timestamp_us);

// The ROI re-read of a decoded crop with PP-OCR recognition.
[[nodiscard]] EvidenceCropRoi reread_evidence_crop(const PpOcrSession& session,
                                                   const PpOcrOptions& options,
                                                   const std::optional<ColorRasterFrame>& crop);

// The whole per-observation work: extract into `scratch_path`, read the bytes,
// decode them back, and re-read them. `scratch_path` is removed afterwards.
[[nodiscard]] EvidenceCropJobOutcome run_evidence_crop_job(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const EvidenceCropJob& job,
    const std::filesystem::path& scratch_path);

// Runs every job, re-reading crops with `roi_options` (the stage's PP-OCR
// options), and returns one outcome per job, in job order; nullopt when the
// jobs must run in the stage itself. `on_progress(done, total)` is called as
// outcomes arrive. Throws DispatchedWorkError when it cannot deliver.
using EvidenceCropDispatcher =
    std::function<std::optional<std::vector<EvidenceCropJobOutcome>>(
        const std::vector<EvidenceCropJob>& jobs, const PpOcrOptions& roi_options,
        const std::function<void(std::size_t done, std::size_t total)>& on_progress)>;

}  // namespace svp::vision
