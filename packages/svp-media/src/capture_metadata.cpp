#include "svp/media/capture_metadata.hpp"

#include "capture_text.hpp"
#include "capture_time.hpp"
#include "isobmff_meta_xml.hpp"
#include "svp/media/capture_location.hpp"
#include "svp/media/media_probe.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace svp::media {
namespace {

constexpr std::string_view kSchemaVersion = "svp-capture-v1";

// Where values come from (CaptureMetadata::sources).
constexpr std::string_view kQuickTime = "quicktime_metadata";
constexpr std::string_view kMxf = "mxf_identification";
constexpr std::string_view kContainer = "container_tags";
constexpr std::array kSources = {kQuickTime, std::string_view("sony_nonrealtime_meta"), kMxf,
                                 kContainer};

// QuickTime metadata keys (Apple, QuickTime File Format: Metadata Keys).
constexpr std::string_view kQtMake = "com.apple.quicktime.make";
constexpr std::string_view kQtModel = "com.apple.quicktime.model";
constexpr std::string_view kQtSoftware = "com.apple.quicktime.software";
constexpr std::string_view kQtCreationDate = "com.apple.quicktime.creationdate";
constexpr std::string_view kQtLocation = "com.apple.quicktime.location.ISO6709";
constexpr std::string_view kQtAccuracy =
    "com.apple.quicktime.location.accuracy.horizontal";

// Tags ffprobe reports from an MP4 `©xyz` atom, and from MXF identification.
constexpr std::string_view kMp4Location = "location";
constexpr std::string_view kMp4LocationEng = "location-eng";
constexpr std::string_view kMxfCompany = "company_name";
constexpr std::string_view kMxfProduct = "product_name";
constexpr std::string_view kMxfVersion = "product_version";
constexpr std::string_view kMxfModified = "modification_date";

const std::string* tag(const nlohmann::json& tags, std::string_view key) {
  if (!tags.is_object()) {
    return nullptr;
  }
  const auto it = tags.find(std::string(key));
  if (it == tags.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
    return nullptr;
  }
  return &it->get_ref<const std::string&>();
}

void note(CaptureMetadata& capture, std::string_view source) {
  for (const auto& known : capture.sources) {
    if (known == source) {
      return;
    }
  }
  capture.sources.emplace_back(source);
}

void set(std::optional<std::string>& field, const std::string* value,
         CaptureMetadata& capture, std::string_view source) {
  if (value == nullptr || field) {
    return;
  }
  if (auto text = trimmed_text(*value)) {
    field = std::move(text);
    note(capture, source);
  }
}

// True when it set the location.
bool set_location_from_iso6709(CaptureMetadata& capture, const std::string* text,
                               std::string_view source) {
  if (text == nullptr || capture.location) {
    return false;
  }
  const auto point = parse_iso6709(*text);
  if (!point) {
    return false;
  }
  capture.location = CaptureLocation{.latitude = point->latitude,
                                     .longitude = point->longitude,
                                     .altitude_m = point->altitude_m};
  note(capture, source);
  return true;
}

void set_recorded(CaptureMetadata& capture, const std::string* text,
                  std::string_view source) {
  if (text == nullptr || capture.recorded) {
    return;
  }
  if (auto recorded = normalized_timestamp(*text)) {
    capture.recorded = std::move(recorded);
    note(capture, source);
  }
}

// A timecode in another form isn't recorded: capture.json would fail its schema.
void set_timecode(CaptureMetadata& capture, const std::string* value) {
  if (value == nullptr || capture.start_timecode) {
    return;
  }
  if (auto text = trimmed_text(*value); text && is_timecode(*text)) {
    capture.start_timecode = std::move(text);
    note(capture, kContainer);
  }
}

bool is_mxf(const nlohmann::json& format) {
  const auto it = format.find("format_name");
  return it != format.end() && it->is_string() && it->get<std::string>() == "mxf";
}

[[noreturn]] void invalid(const std::string& member, const std::string& why) {
  throw std::invalid_argument("capture.json: " + member + " " + why);
}

// `value` as an object with only `allowed` members.
const nlohmann::json& object_of(const nlohmann::json& value, const std::string& member,
                                std::initializer_list<std::string_view> allowed) {
  if (!value.is_object()) {
    invalid(member, "must be an object");
  }
  for (const auto& [name, unused] : value.items()) {
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      invalid(member.empty() ? name : member + "." + name, "is not a capture member");
    }
  }
  return value;
}

// A member that must be text as capture_metadata_to_json writes it: trimmed
// and not empty.
std::optional<std::string> text_of(const nlohmann::json& object, std::string_view name,
                                   const std::string& member) {
  const auto it = object.find(std::string(name));
  if (it == object.end()) {
    return std::nullopt;
  }
  if (!it->is_string() || trimmed_text(it->get_ref<const std::string&>()) !=
                              it->get_ref<const std::string&>()) {
    invalid(member, "must be text without surrounding whitespace");
  }
  return it->get<std::string>();
}

std::optional<double> number_of(const nlohmann::json& object, std::string_view name,
                                const std::string& member) {
  const auto it = object.find(std::string(name));
  if (it == object.end()) {
    return std::nullopt;
  }
  if (!it->is_number()) {
    invalid(member, "must be a number");
  }
  return it->get<double>();
}

}  // namespace

bool CaptureMetadata::empty() const {
  return !make && !model && !software && !serial_number && !lens_model && !recorded &&
         !start_timecode && !location;
}

void merge_ffprobe_json(CaptureMetadata& capture, const nlohmann::json& ffprobe) {
  const nlohmann::json empty_object = nlohmann::json::object();
  const auto format_it = ffprobe.find("format");
  const nlohmann::json& format =
      format_it != ffprobe.end() && format_it->is_object() ? *format_it : empty_object;
  const auto tags_it = format.find("tags");
  const nlohmann::json& tags =
      tags_it != format.end() && tags_it->is_object() ? *tags_it : empty_object;

  // A camera's own keys first: they describe the shot, not the file.
  set(capture.make, tag(tags, kQtMake), capture, kQuickTime);
  set(capture.model, tag(tags, kQtModel), capture, kQuickTime);
  set(capture.software, tag(tags, kQtSoftware), capture, kQuickTime);
  set_recorded(capture, tag(tags, kQtCreationDate), kQuickTime);
  if (set_location_from_iso6709(capture, tag(tags, kQtLocation), kQuickTime)) {
    if (const auto* accuracy = tag(tags, kQtAccuracy)) {
      capture.location->horizontal_accuracy_m = parse_unsigned_decimal(*accuracy);
    }
  }

  if (is_mxf(format)) {
    set(capture.make, tag(tags, kMxfCompany), capture, kMxf);
    set(capture.model, tag(tags, kMxfProduct), capture, kMxf);
    set(capture.software, tag(tags, kMxfVersion), capture, kMxf);
  }

  set_location_from_iso6709(capture, tag(tags, kMp4Location), kContainer);
  set_location_from_iso6709(capture, tag(tags, kMp4LocationEng), kContainer);
  set_recorded(capture, tag(tags, "creation_time"), kContainer);
  if (is_mxf(format)) {
    set_recorded(capture, tag(tags, kMxfModified), kMxf);
  }

  set_timecode(capture, tag(tags, "timecode"));
  if (const auto streams = ffprobe.find("streams");
      streams != ffprobe.end() && streams->is_array()) {
    for (const auto& stream : *streams) {
      if (capture.start_timecode) {
        break;
      }
      const auto stream_tags = stream.find("tags");
      if (stream_tags != stream.end()) {
        set_timecode(capture, tag(*stream_tags, "timecode"));
      }
    }
  }
}

CaptureMetadata read_capture_metadata(const std::filesystem::path& source_path,
                                      const std::filesystem::path& ffprobe_path) {
  CaptureMetadata capture;
  // The camera's own record of the shot first; container tags, which record
  // the file, only fill what it doesn't state.
  if (const auto xml = read_isobmff_meta_xml(source_path);
      xml && xml->find("NonRealTimeMeta") != std::string::npos) {
    merge_sony_nonrealtime_meta(capture, *xml);
  }
  try {
    merge_ffprobe_json(capture, run_ffprobe_json(source_path, ffprobe_path));
  } catch (const std::exception&) {
    // A file ffprobe can't read keeps what its NonRealTimeMeta stated.
  }
  return capture;
}

nlohmann::json capture_metadata_to_json(const CaptureMetadata& capture) {
  nlohmann::json out = {{"schema_version", kSchemaVersion}};
  nlohmann::json camera = nlohmann::json::object();
  if (capture.make) {
    camera["make"] = *capture.make;
  }
  if (capture.model) {
    camera["model"] = *capture.model;
  }
  if (capture.software) {
    camera["software"] = *capture.software;
  }
  if (capture.serial_number) {
    camera["serial_number"] = *capture.serial_number;
  }
  if (!camera.empty()) {
    out["camera"] = std::move(camera);
  }
  if (capture.lens_model) {
    out["lens"] = {{"model", *capture.lens_model}};
  }
  if (capture.recorded) {
    out["recorded"] = *capture.recorded;
  }
  if (capture.start_timecode) {
    out["start_timecode"] = *capture.start_timecode;
  }
  if (capture.location) {
    const CaptureLocation& l = *capture.location;
    nlohmann::json location = {{"latitude", l.latitude}, {"longitude", l.longitude}};
    if (l.altitude_m) {
      location["altitude_m"] = *l.altitude_m;
    }
    if (l.horizontal_accuracy_m) {
      location["horizontal_accuracy_m"] = *l.horizontal_accuracy_m;
    }
    if (l.fix) {
      location["fix"] = *l.fix;
    }
    if (l.fixed_utc) {
      location["fixed_utc"] = *l.fixed_utc;
    }
    out["location"] = std::move(location);
  }
  out["sources"] = capture.sources;
  return out;
}

CaptureMetadata capture_metadata_from_json(const nlohmann::json& value) {
  const nlohmann::json& root = object_of(
      value, "", {"schema_version", "camera", "lens", "recorded", "start_timecode", "location",
                  "sources"});
  const auto version = root.find("schema_version");
  if (version == root.end() || !version->is_string() ||
      version->get_ref<const std::string&>() != kSchemaVersion) {
    invalid("schema_version", "must be " + std::string(kSchemaVersion));
  }
  CaptureMetadata capture;
  if (const auto camera = root.find("camera"); camera != root.end()) {
    object_of(*camera, "camera", {"make", "model", "software", "serial_number"});
    capture.make = text_of(*camera, "make", "camera.make");
    capture.model = text_of(*camera, "model", "camera.model");
    capture.software = text_of(*camera, "software", "camera.software");
    capture.serial_number = text_of(*camera, "serial_number", "camera.serial_number");
  }
  if (const auto lens = root.find("lens"); lens != root.end()) {
    object_of(*lens, "lens", {"model"});
    capture.lens_model = text_of(*lens, "model", "lens.model");
    if (!capture.lens_model) {
      invalid("lens.model", "is missing");
    }
  }
  capture.recorded = text_of(root, "recorded", "recorded");
  if (capture.recorded && normalized_timestamp(*capture.recorded) != capture.recorded) {
    invalid("recorded", "must be ISO 8601 with a UTC offset");
  }
  capture.start_timecode = text_of(root, "start_timecode", "start_timecode");
  if (capture.start_timecode && !is_timecode(*capture.start_timecode)) {
    invalid("start_timecode", "is not a timecode");
  }
  if (const auto location = root.find("location"); location != root.end()) {
    object_of(*location, "location",
              {"latitude", "longitude", "altitude_m", "horizontal_accuracy_m", "fix",
               "fixed_utc"});
    const auto latitude = number_of(*location, "latitude", "location.latitude");
    const auto longitude = number_of(*location, "longitude", "location.longitude");
    if (!latitude || *latitude < -90.0 || *latitude > 90.0) {
      invalid("location.latitude", "must be within -90 to 90");
    }
    if (!longitude || *longitude < -180.0 || *longitude > 180.0) {
      invalid("location.longitude", "must be within -180 to 180");
    }
    CaptureLocation where{.latitude = *latitude, .longitude = *longitude};
    where.altitude_m = number_of(*location, "altitude_m", "location.altitude_m");
    where.horizontal_accuracy_m =
        number_of(*location, "horizontal_accuracy_m", "location.horizontal_accuracy_m");
    if (where.horizontal_accuracy_m && *where.horizontal_accuracy_m < 0.0) {
      invalid("location.horizontal_accuracy_m", "must not be negative");
    }
    where.fix = text_of(*location, "fix", "location.fix");
    if (where.fix && *where.fix != "2d" && *where.fix != "3d") {
      invalid("location.fix", "must be 2d or 3d");
    }
    where.fixed_utc = text_of(*location, "fixed_utc", "location.fixed_utc");
    if (where.fixed_utc && (normalized_timestamp(*where.fixed_utc) != where.fixed_utc ||
                            where.fixed_utc->back() != 'Z')) {
      invalid("location.fixed_utc", "must be ISO 8601 in UTC");
    }
    capture.location = std::move(where);
  }
  const auto sources = root.find("sources");
  if (sources == root.end() || !sources->is_array()) {
    invalid("sources", "must be an array");
  }
  for (const auto& source : *sources) {
    if (!source.is_string() ||
        std::find(kSources.begin(), kSources.end(), source.get<std::string>()) ==
            kSources.end()) {
      invalid("sources", "names an unknown source: " + source.dump());
    }
    capture.sources.push_back(source.get<std::string>());
  }
  return capture;
}

}  // namespace svp::media
