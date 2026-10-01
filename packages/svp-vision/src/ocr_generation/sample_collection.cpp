#include "ocr_generation_internal.hpp"

#include <iomanip>
#include <sstream>

namespace svp::vision::ocr_generation_internal {
namespace {

// The frame ID the streaming decoder gives the n-th decoded frame (0-based)
// when no frame catalog assigns IDs.
std::string uncatalogued_frame_id(std::size_t decoded_index) {
  std::ostringstream oss;
  oss << "frame_" << std::setw(6) << std::setfill('0') << (decoded_index + 1);
  return oss.str();
}

}  // namespace

std::vector<std::optional<OcrSampleFrame>> register_ocr_sample_frames(
    const std::vector<OcrSampleDetections>& samples,
    FrameCatalog* frame_catalog) {
  std::vector<std::optional<OcrSampleFrame>> frames;
  frames.reserve(samples.size());
  std::size_t decoded = 0;
  for (const OcrSampleDetections& sample : samples) {
    if (sample.status == OcrSampleStatus::decode_missed) {
      frames.emplace_back(std::nullopt);
      continue;
    }
    OcrSampleFrame frame;
    frame.frame_index = decoded;
    if (frame_catalog != nullptr) {
      frame.frame_id = frame_catalog->register_frame(
          sample.timestamp_us, kOcrFramePurpose, decoded == 0);
      if (const auto index = frame_catalog->get_frame_index(sample.timestamp_us)) {
        frame.frame_index = *index;
      }
    } else {
      frame.frame_id = uncatalogued_frame_id(decoded);
    }
    frames.emplace_back(std::move(frame));
    ++decoded;
  }
  return frames;
}

DecodedCanonicalFrames summarize_ocr_sample_decoding(
    const std::vector<OcrSampleDetections>& samples) {
  DecodedCanonicalFrames status;
  status.decoding_attempted = true;
  status.frames_attempted = static_cast<int>(samples.size());
  for (const OcrSampleDetections& sample : samples) {
    if (sample.status != OcrSampleStatus::decode_missed) {
      ++status.frames_decoded;
      continue;
    }
    ++status.frames_missed;
    if (status.skipped_reason.empty()) {
      status.skipped_reason = "frame miss at " +
          std::to_string(sample.timestamp_us) + "us: " + sample.error;
    }
  }
  status.decoding_succeeded = status.frames_decoded > 0;
  return status;
}

void collect_ocr_sample(const OcrSampleDetections& sample,
                        const OcrSampleFrame& frame,
                        CollectedOcrFrames& collected) {
  if (sample.status == OcrSampleStatus::decode_missed) return;
  ++collected.processed_frame_count;
  if (collected.processed_frame_width == 0 &&
      collected.processed_frame_height == 0) {
    collected.processed_frame_width = sample.frame_width;
    collected.processed_frame_height = sample.frame_height;
  }

  switch (sample.status) {
    case OcrSampleStatus::decode_missed:
      return;
    case OcrSampleStatus::frame_invalid:
      collected.frame_diagnostics.push_back({
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", sample.timestamp_us},
          {"extraction_succeeded", false},
          {"extraction_error", sample.error},
          {"pp_ocr_attempted", false}
      });
      collected.any_frame_failed = true;
      if (collected.failure_reason_details.empty()) {
        collected.failure_reason_details =
            sanitize_utf8("Invalid frame data for " + frame.frame_id);
      }
      return;
    case OcrSampleStatus::ocr_failed:
      collected.frame_diagnostics.push_back({
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", sample.timestamp_us},
          {"extraction_succeeded", true},
          {"pp_ocr_attempted", true},
          {"pp_ocr_succeeded", false},
          {"pp_ocr_error", sanitize_utf8(sample.error)}
      });
      collected.any_frame_failed = true;
      if (collected.failure_reason_details.empty()) {
        collected.failure_reason_details = sanitize_utf8(
            "PP-OCR failed on frame " + frame.frame_id + ": " + sample.error);
      }
      return;
    case OcrSampleStatus::ok:
      break;
  }

  collected.frame_diagnostics.push_back({
      {"frame_id", sanitize_utf8(frame.frame_id)},
      {"timestamp_us", sample.timestamp_us},
      {"extraction_succeeded", true},
      {"pp_ocr_attempted", true},
      {"pp_ocr_succeeded", true},
      {"detection_count", sample.detections.size()}
  });

  for (const OcrTextDetection& det : sample.detections) {
    if (det.bbox_right <= det.bbox_left || det.bbox_bottom <= det.bbox_top) {
      continue;
    }
    FrameDetection fdet;
    fdet.frame_id = frame.frame_id;
    fdet.timestamp_us = sample.timestamp_us;
    fdet.frame_index = frame.frame_index;
    fdet.frame_width = sample.frame_width;
    fdet.frame_height = sample.frame_height;
    fdet.raw_text = det.text;
    fdet.confidence = det.confidence;
    fdet.bbox_left = det.bbox_left;
    fdet.bbox_top = det.bbox_top;
    fdet.bbox_right = det.bbox_right;
    fdet.bbox_bottom = det.bbox_bottom;
    collected.detections.push_back(std::move(fdet));
  }
}

}  // namespace svp::vision::ocr_generation_internal
