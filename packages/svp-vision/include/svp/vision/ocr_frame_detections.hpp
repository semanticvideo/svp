#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision {

// Per-sample result of OCR frame processing: the payload of one
// ocr.frame_batch task (plan §4.2), one record per sample ordinal. It carries
// only what decoding and PP-OCR produced. Frame IDs, frame indices, and every
// other counter are assigned by the reducer on the coordinator from sample
// order (plan §4.5), so a record is a pure function of the source bytes, the
// timestamp, the decode size, and the PP-OCR parameters.

enum class OcrSampleStatus {
  // Decoded and PP-OCR ran; `detections` holds its output.
  ok,
  // ffmpeg produced no frame at this timestamp. Tolerated: the sample is
  // skipped, as a decode miss always was.
  decode_missed,
  // A decoded frame had no usable pixels.
  frame_invalid,
  // PP-OCR threw on this frame; the stage reports OCR as failed.
  ocr_failed,
  // OCR could not start for the whole batch on the coordinator (PP-OCR did
  // not load, or ffmpeg could not decode at all). Never a frame result: the
  // OCR stage then runs exactly as a build without frame batches would
  // (run_vision_ocr_stage), which reports why. Workers never emit it.
  not_started,
};

[[nodiscard]] std::string_view ocr_sample_status_name(OcrSampleStatus status);

// One PP-OCR text box exactly as the recognizer returned it (no filtering).
struct OcrTextDetection {
  std::string text;
  double confidence = 0.0;
  int bbox_left = 0;
  int bbox_top = 0;
  int bbox_right = 0;
  int bbox_bottom = 0;

  bool operator==(const OcrTextDetection&) const = default;
};

struct OcrSampleDetections {
  std::uint64_t sample_ordinal = 0;
  std::int64_t timestamp_us = 0;
  OcrSampleStatus status = OcrSampleStatus::ok;
  // Why the sample is not `ok`: ffmpeg's decode error, or the frame/PP-OCR
  // error (verbatim, so it may itself be empty). Always empty when ok.
  std::string error;
  // Decoded frame size; 0 x 0 for decode_missed.
  int frame_width = 0;
  int frame_height = 0;
  // PP-OCR output in detector order; empty unless status is ok.
  std::vector<OcrTextDetection> detections;

  bool operator==(const OcrSampleDetections&) const = default;
};

// Malformed, non-canonical, or inconsistent payload bytes.
class OcrFrameDetectionsCodecError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Canonical JSON form of one record (sorted keys, integers for time, sizes,
// and boxes; confidence in shortest round-trip floating-point form):
//   ok:            {"detections":[{"bbox":[l,t,r,b],"confidence":c,"text":s}],
//                   "frame_height","frame_width","sample_ordinal","status",
//                   "timestamp_us"}
//   decode_missed: {"error","sample_ordinal","status","timestamp_us"}
//   frame_invalid,
//   ocr_failed:    {"error","frame_height","frame_width","sample_ordinal",
//                   "status","timestamp_us"}
// Throws OcrFrameDetectionsCodecError for a record that breaks the status
// rules above or holds a non-finite confidence.
[[nodiscard]] nlohmann::json ocr_sample_detections_to_json(
    const OcrSampleDetections& record);
// Strict inverse: unknown, missing, or mistyped fields are rejected.
[[nodiscard]] OcrSampleDetections ocr_sample_detections_from_json(
    const nlohmann::json& value);

// JSONL payload: each record's canonical JSON followed by "\n", in the given
// order. Byte-stable: equal records always encode to equal bytes, which is
// what lets a coordinator compare the output of two executions by digest.
[[nodiscard]] std::string encode_ocr_sample_detections_jsonl(
    std::span<const OcrSampleDetections> records);
// Strict inverse: every line must be exactly the canonical encoding of its
// record and end in "\n". Empty input is zero records.
[[nodiscard]] std::vector<OcrSampleDetections>
decode_ocr_sample_detections_jsonl(std::string_view bytes);

}  // namespace svp::vision
