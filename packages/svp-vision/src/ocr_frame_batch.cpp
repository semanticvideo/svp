#include "svp/vision/ocr_frame_batch.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"

#include <exception>
#include <utility>

namespace svp::vision {
namespace {

// Diagnostic label for a frame that has no package frame ID yet (IDs are
// assigned by the reducer). PP-OCR only uses it in memory diagnostics.
std::string sample_label(std::uint64_t ordinal) {
  return "ocr_sample_" + std::to_string(ordinal);
}

OcrSampleDetections decode_missed(const OcrSample& sample, std::string error) {
  OcrSampleDetections record;
  record.sample_ordinal = sample.ordinal;
  record.timestamp_us = sample.timestamp_us;
  record.status = OcrSampleStatus::decode_missed;
  record.error = std::move(error);
  return record;
}

}  // namespace

OcrSampleDetections run_ocr_on_decoded_frame(const PpOcrSession& session,
                                             const PpOcrOptions& options,
                                             const ColorRasterFrame& frame,
                                             std::uint64_t sample_ordinal) {
  OcrSampleDetections record;
  record.sample_ordinal = sample_ordinal;
  record.timestamp_us = frame.timestamp_us;
  record.frame_width = frame.width;
  record.frame_height = frame.height;

  if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
    record.status = OcrSampleStatus::frame_invalid;
    record.error = "Empty or invalid frame pixels";
    return record;
  }

  PpOcrFrameResult result;
  try {
    result = run_pp_ocr_on_frame(session, options, frame);
  } catch (const std::exception& error) {
    record.status = OcrSampleStatus::ocr_failed;
    record.error = sanitize_utf8(error.what());
    return record;
  }

  record.status = OcrSampleStatus::ok;
  record.detections.reserve(result.detections.size());
  for (PpOcrDetection& detection : result.detections) {
    record.detections.push_back(OcrTextDetection{
        .text = std::move(detection.text),
        .confidence = detection.score,
        .bbox_left = detection.bbox_left,
        .bbox_top = detection.bbox_top,
        .bbox_right = detection.bbox_right,
        .bbox_bottom = detection.bbox_bottom,
    });
  }
  return record;
}

OcrFrameBatchOutcome run_ocr_frame_batch(const PpOcrSession& session,
                                         const PpOcrOptions& options,
                                         const OcrFrameBatchRequest& request,
                                         const OcrFrameBatchHooks& hooks) {
  OcrFrameBatchOutcome outcome;
  if (!ffmpeg_executable_available(request.ffmpeg_path)) {
    outcome.skipped_reason = "ffmpeg not found at: " + request.ffmpeg_path.string();
    return outcome;
  }
  if (request.frame_width <= 0 || request.frame_height <= 0) {
    outcome.skipped_reason = "target frame dimensions are not positive";
    return outcome;
  }
  outcome.decoding_attempted = true;
  outcome.samples.reserve(request.samples.size());

  for (const OcrSample& sample : request.samples) {
    if (hooks.before_sample) hooks.before_sample();
    std::string decode_error;
    std::vector<Srgb8Pixel> pixels = decode_rgb_frame_at(
        request.ffmpeg_path, request.source_path, sample.timestamp_us,
        request.frame_width, request.frame_height, decode_error);
    if (pixels.empty()) {
      outcome.samples.push_back(decode_missed(sample, std::move(decode_error)));
      continue;
    }
    if (hooks.on_sample_decoded) hooks.on_sample_decoded(sample);

    const ColorRasterFrame frame{
        sample_label(sample.ordinal),
        sample.timestamp_us,
        request.frame_width,
        request.frame_height,
        false,
        std::move(pixels),
    };
    outcome.samples.push_back(
        run_ocr_on_decoded_frame(session, options, frame, sample.ordinal));
  }
  return outcome;
}

}  // namespace svp::vision
