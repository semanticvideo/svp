#include "svp/vision/ocr_frame_detections.hpp"

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <type_traits>

namespace svp::vision {
namespace {

using Json = nlohmann::json;

// Deepest nesting of a valid record: record > detections > detection > bbox.
// Lines nesting deeper are rejected before parsing, so corrupt or hostile
// bytes cannot build a deep DOM.
constexpr std::size_t kMaxRecordDepth = 4;

constexpr std::array kStatuses = {
    OcrSampleStatus::ok,
    OcrSampleStatus::decode_missed,
    OcrSampleStatus::frame_invalid,
    OcrSampleStatus::ocr_failed,
};

[[noreturn]] void fail(const std::string& message) {
  throw OcrFrameDetectionsCodecError("ocr frame detections: " + message);
}

std::optional<OcrSampleStatus> parse_status(std::string_view name) {
  for (const OcrSampleStatus status : kStatuses) {
    if (ocr_sample_status_name(status) == name) return status;
  }
  return std::nullopt;
}

bool has_frame_size(OcrSampleStatus status) {
  return status != OcrSampleStatus::decode_missed;
}

std::string dump_canonical(const Json& value) {
  try {
    return value.dump(-1, ' ', false, Json::error_handler_t::strict);
  } catch (const Json::type_error& error) {
    fail(std::string("text is not valid UTF-8: ") + error.what());
  }
}

void require_fields(const Json& value,
                    const std::set<std::string>& expected,
                    const std::string& where) {
  if (!value.is_object()) fail(where + " must be an object");
  for (const auto& [key, unused] : value.items()) {
    if (!expected.contains(key)) fail(where + " has unknown field `" + key + "`");
  }
  for (const std::string& key : expected) {
    if (!value.contains(key)) fail(where + " is missing field `" + key + "`");
  }
}

template <typename Integer>
Integer require_integer(const Json& value, const std::string& where) {
  if (value.is_number_unsigned()) {
    const auto parsed = value.get<std::uint64_t>();
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
      fail(where + " is out of range");
    }
    return static_cast<Integer>(parsed);
  }
  if (value.is_number_integer()) {
    const auto parsed = value.get<std::int64_t>();
    if constexpr (std::is_unsigned_v<Integer>) {
      fail(where + " must not be negative");
    } else {
      if (parsed < static_cast<std::int64_t>(std::numeric_limits<Integer>::min()) ||
          parsed > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
        fail(where + " is out of range");
      }
      return static_cast<Integer>(parsed);
    }
  }
  fail(where + " must be an integer");
}

std::string require_string(const Json& value, const std::string& where) {
  if (!value.is_string()) fail(where + " must be a string");
  return value.get<std::string>();
}

void validate_record(const OcrSampleDetections& record) {
  if (record.timestamp_us < 0) fail("timestamp_us must not be negative");
  const bool ok = record.status == OcrSampleStatus::ok;
  if (ok && !record.error.empty()) fail("an ok sample has no error");
  if (!ok && !record.detections.empty()) {
    fail("only an ok sample carries detections");
  }
  if (!has_frame_size(record.status) &&
      (record.frame_width != 0 || record.frame_height != 0)) {
    fail("a decode_missed sample has no frame size");
  }
  for (const OcrTextDetection& detection : record.detections) {
    if (!std::isfinite(detection.confidence)) {
      fail("confidence must be finite");
    }
  }
}

Json detection_to_json(const OcrTextDetection& detection) {
  return Json{
      {"bbox", Json::array({detection.bbox_left, detection.bbox_top,
                            detection.bbox_right, detection.bbox_bottom})},
      {"confidence", detection.confidence},
      {"text", detection.text},
  };
}

OcrTextDetection detection_from_json(const Json& value, const std::string& where) {
  require_fields(value, {"bbox", "confidence", "text"}, where);
  const Json& bbox = value.at("bbox");
  if (!bbox.is_array() || bbox.size() != 4) {
    fail(where + ".bbox must be an array of four integers");
  }
  const Json& confidence = value.at("confidence");
  // The encoder always writes a double, so an integer here is non-canonical.
  if (!confidence.is_number_float()) {
    fail(where + ".confidence must be a floating-point number");
  }
  return OcrTextDetection{
      .text = require_string(value.at("text"), where + ".text"),
      .confidence = confidence.get<double>(),
      .bbox_left = require_integer<int>(bbox[0], where + ".bbox[0]"),
      .bbox_top = require_integer<int>(bbox[1], where + ".bbox[1]"),
      .bbox_right = require_integer<int>(bbox[2], where + ".bbox[2]"),
      .bbox_bottom = require_integer<int>(bbox[3], where + ".bbox[3]"),
  };
}

void require_bounded_depth(std::string_view line) {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char character : line) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '[') {
      if (++depth > kMaxRecordDepth) fail("record nests too deeply");
    } else if ((character == '}' || character == ']') && depth > 0) {
      --depth;
    }
  }
}

}  // namespace

std::string_view ocr_sample_status_name(OcrSampleStatus status) {
  switch (status) {
    case OcrSampleStatus::ok:
      return "ok";
    case OcrSampleStatus::decode_missed:
      return "decode_missed";
    case OcrSampleStatus::frame_invalid:
      return "frame_invalid";
    case OcrSampleStatus::ocr_failed:
      return "ocr_failed";
  }
  return "unknown";
}

Json ocr_sample_detections_to_json(const OcrSampleDetections& record) {
  validate_record(record);
  Json value{
      {"sample_ordinal", record.sample_ordinal},
      {"status", std::string(ocr_sample_status_name(record.status))},
      {"timestamp_us", record.timestamp_us},
  };
  if (record.status == OcrSampleStatus::ok) {
    Json detections = Json::array();
    for (const OcrTextDetection& detection : record.detections) {
      detections.push_back(detection_to_json(detection));
    }
    value["detections"] = std::move(detections);
  } else {
    value["error"] = record.error;
  }
  if (has_frame_size(record.status)) {
    value["frame_width"] = record.frame_width;
    value["frame_height"] = record.frame_height;
  }
  return value;
}

OcrSampleDetections ocr_sample_detections_from_json(const Json& value) {
  if (!value.is_object() || !value.contains("status")) {
    fail("record must be an object with a status");
  }
  const auto status =
      parse_status(require_string(value.at("status"), "record.status"));
  if (!status) fail("record.status is unknown");

  std::set<std::string> fields = {"sample_ordinal", "status", "timestamp_us"};
  fields.insert(*status == OcrSampleStatus::ok ? "detections" : "error");
  if (has_frame_size(*status)) {
    fields.insert("frame_width");
    fields.insert("frame_height");
  }
  require_fields(value, fields, "record");

  OcrSampleDetections record;
  record.status = *status;
  record.sample_ordinal =
      require_integer<std::uint64_t>(value.at("sample_ordinal"), "record.sample_ordinal");
  record.timestamp_us =
      require_integer<std::int64_t>(value.at("timestamp_us"), "record.timestamp_us");
  if (has_frame_size(*status)) {
    record.frame_width = require_integer<int>(value.at("frame_width"), "record.frame_width");
    record.frame_height =
        require_integer<int>(value.at("frame_height"), "record.frame_height");
  }
  if (*status == OcrSampleStatus::ok) {
    const Json& detections = value.at("detections");
    if (!detections.is_array()) fail("record.detections must be an array");
    for (std::size_t index = 0; index < detections.size(); ++index) {
      record.detections.push_back(detection_from_json(
          detections[index], "record.detections[" + std::to_string(index) + "]"));
    }
  } else {
    record.error = require_string(value.at("error"), "record.error");
  }
  validate_record(record);
  return record;
}

std::string encode_ocr_sample_detections_jsonl(
    std::span<const OcrSampleDetections> records) {
  std::string bytes;
  for (const OcrSampleDetections& record : records) {
    bytes += dump_canonical(ocr_sample_detections_to_json(record));
    bytes += '\n';
  }
  return bytes;
}

std::vector<OcrSampleDetections> decode_ocr_sample_detections_jsonl(
    std::string_view bytes) {
  std::vector<OcrSampleDetections> records;
  std::size_t start = 0;
  while (start < bytes.size()) {
    const std::size_t end = bytes.find('\n', start);
    if (end == std::string_view::npos) fail("last record does not end in a newline");
    const std::string_view line = bytes.substr(start, end - start);
    const std::string where = "line " + std::to_string(records.size() + 1);
    require_bounded_depth(line);
    Json value;
    try {
      value = Json::parse(line);
    } catch (const Json::parse_error& error) {
      fail(where + " is not JSON: " + error.what());
    }
    OcrSampleDetections record = ocr_sample_detections_from_json(value);
    if (dump_canonical(ocr_sample_detections_to_json(record)) != line) {
      fail(where + " is not in canonical form");
    }
    records.push_back(std::move(record));
    start = end + 1;
  }
  return records;
}

}  // namespace svp::vision
