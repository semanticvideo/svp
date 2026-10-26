#pragma once

// Capture metadata: what the camera recorded about a shot in the source file's
// own metadata (spec Section 18.3). Every field is optional: a file carries
// what its camera wrote, and nothing is inferred or estimated.
//
// Read from the container tags ffprobe reports (QuickTime metadata keys, MXF
// identification, creation time, timecode) and from Sony's NonRealTimeMeta XML
// in an ISO BMFF file's `meta` box. Per-frame metadata tracks are not read.

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::media {

// Where the camera was, in WGS-84 decimal degrees (north and east positive).
struct CaptureLocation {
  double latitude = 0.0;
  double longitude = 0.0;
  std::optional<double> altitude_m;
  std::optional<double> horizontal_accuracy_m;
  std::optional<std::string> fix;        // "2d" or "3d", when the camera states it
  std::optional<std::string> fixed_utc;  // when the GPS fix was taken, when stated
};

struct CaptureMetadata {
  std::optional<std::string> make;
  std::optional<std::string> model;
  std::optional<std::string> software;  // the camera's firmware or OS version
  std::optional<std::string> serial_number;  // the camera body's, as the camera states it
  std::optional<std::string> lens_model;
  // ISO 8601, with the camera's UTC offset when it records one, else UTC.
  std::optional<std::string> recorded;
  std::optional<std::string> start_timecode;
  std::optional<CaptureLocation> location;
  // Which metadata each value came from, in the order first used, e.g.
  // "quicktime_metadata", "sony_nonrealtime_meta", "mxf_identification".
  std::vector<std::string> sources;

  [[nodiscard]] bool empty() const;
};

// Each merge fills only the values `capture` doesn't have yet, so the order of
// merges is the order of precedence.

// Adds what ffprobe's `-show_format -show_streams` JSON states.
void merge_ffprobe_json(CaptureMetadata& capture, const nlohmann::json& ffprobe);

// Adds what Sony's NonRealTimeMeta XML states.
void merge_sony_nonrealtime_meta(CaptureMetadata& capture, std::string_view xml);

// Reads a source file's capture metadata. Never throws: a file that can't be
// probed or read has no capture metadata.
[[nodiscard]] CaptureMetadata read_capture_metadata(
    const std::filesystem::path& source_path,
    const std::filesystem::path& ffprobe_path = "ffprobe");

// provenance/capture.json (capture.schema.json); absent fields are omitted.
[[nodiscard]] nlohmann::json capture_metadata_to_json(const CaptureMetadata& capture);

// The capture metadata a provenance/capture.json document states. Throws
// std::invalid_argument naming the member for a document that isn't one
// (unknown members included).
[[nodiscard]] CaptureMetadata capture_metadata_from_json(const nlohmann::json& value);

}  // namespace svp::media
