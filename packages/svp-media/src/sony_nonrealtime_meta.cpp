// Sony's NonRealTimeMeta XML (urn:schemas-professionalDisc:nonRealTimeMeta),
// which Sony cameras write into an ISO BMFF file's `meta` box: the camera
// (`Device`), the lens (`Lens`), when recording started (`CreationDate`), and
// GPS as EXIF GPS fields in its `ExifGPS` group.
//
// Only start tags and their attributes are read; element text isn't used by
// any of these, so a full XML parser isn't needed.

#include "capture_text.hpp"
#include "capture_time.hpp"
#include "svp/media/capture_location.hpp"
#include "svp/media/capture_metadata.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::media {
namespace {

constexpr std::string_view kSource = "sony_nonrealtime_meta";
constexpr std::string_view kGpsGroup = "ExifGPS";
constexpr double kMaxLatitude = 90.0;
constexpr double kMaxLongitude = 180.0;

struct Tag {
  std::string name;  // without a namespace prefix
  bool closing = false;
  std::map<std::string, std::string, std::less<>> attributes;
};

std::string decode_entities(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const std::size_t end = text.find(';', i);
    if (end == std::string_view::npos) {
      out.push_back(text[i]);
      continue;
    }
    const std::string_view entity = text.substr(i + 1, end - i - 1);
    if (entity == "amp") {
      out.push_back('&');
    } else if (entity == "lt") {
      out.push_back('<');
    } else if (entity == "gt") {
      out.push_back('>');
    } else if (entity == "quot") {
      out.push_back('"');
    } else if (entity == "apos") {
      out.push_back('\'');
    } else {
      out.append(text.substr(i, end - i + 1));  // left as written
    }
    i = end;
  }
  return out;
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// The start and end tags in document order. Comments, declarations and
// processing instructions are skipped.
std::vector<Tag> scan_tags(std::string_view xml) {
  std::vector<Tag> tags;
  std::size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string_view::npos) {
    if (xml.substr(pos, 4) == "<!--") {
      const std::size_t end = xml.find("-->", pos);
      if (end == std::string_view::npos) {
        break;
      }
      pos = end + 3;
      continue;
    }
    if (pos + 1 < xml.size() && (xml[pos + 1] == '?' || xml[pos + 1] == '!')) {
      pos = xml.find('>', pos);
      if (pos == std::string_view::npos) {
        break;
      }
      continue;
    }
    Tag tag;
    std::size_t i = pos + 1;
    if (i < xml.size() && xml[i] == '/') {
      tag.closing = true;
      ++i;
    }
    const std::size_t name_start = i;
    while (i < xml.size() && !is_space(xml[i]) && xml[i] != '/' && xml[i] != '>') {
      ++i;
    }
    std::string_view name = xml.substr(name_start, i - name_start);
    if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
      name.remove_prefix(colon + 1);
    }
    tag.name = std::string(name);
    while (i < xml.size() && xml[i] != '>') {
      if (is_space(xml[i]) || xml[i] == '/') {
        ++i;
        continue;
      }
      const std::size_t key_start = i;
      while (i < xml.size() && xml[i] != '=' && !is_space(xml[i]) && xml[i] != '>') {
        ++i;
      }
      const std::string key(xml.substr(key_start, i - key_start));
      while (i < xml.size() && is_space(xml[i])) {
        ++i;
      }
      if (i >= xml.size() || xml[i] != '=') {
        continue;
      }
      ++i;
      while (i < xml.size() && is_space(xml[i])) {
        ++i;
      }
      if (i >= xml.size() || (xml[i] != '"' && xml[i] != '\'')) {
        continue;
      }
      const char quote = xml[i++];
      const std::size_t value_end = xml.find(quote, i);
      if (value_end == std::string_view::npos) {
        return tags;
      }
      tag.attributes[key] = decode_entities(xml.substr(i, value_end - i));
      i = value_end + 1;
    }
    tags.push_back(std::move(tag));
    pos = i;
  }
  return tags;
}

const std::string* attribute(const Tag& tag, std::string_view key) {
  const auto it = tag.attributes.find(key);
  return it == tag.attributes.end() || it->second.empty() ? nullptr : &it->second;
}

void note_source(CaptureMetadata& capture) {
  for (const auto& source : capture.sources) {
    if (source == kSource) {
      return;
    }
  }
  capture.sources.emplace_back(kSource);
}

void set_text(CaptureMetadata& capture, std::optional<std::string>& field,
              std::string_view value) {
  if (field) {
    return;
  }
  if (auto text = trimmed_text(value)) {
    field = std::move(text);
    note_source(capture);
  }
}

// EXIF GPS, as the ExifGPS group's items state it. None unless the fix is
// valid, the datum is WGS-84 and both coordinates read.
std::optional<CaptureLocation> gps_location(
    const std::map<std::string, std::string, std::less<>>& items) {
  const auto item = [&items](std::string_view name) -> const std::string* {
    const auto it = items.find(name);
    return it == items.end() ? nullptr : &it->second;
  };
  // EXIF GPSStatus: "A" is a measurement in progress, "V" means none.
  if (const auto* status = item("Status"); status != nullptr && *status != "A") {
    return std::nullopt;
  }
  if (const auto* datum = item("MapDatum");
      datum != nullptr && *datum != "WGS-84" && *datum != "WGS84") {
    return std::nullopt;
  }
  const auto* latitude = item("Latitude");
  const auto* latitude_ref = item("LatitudeRef");
  const auto* longitude = item("Longitude");
  const auto* longitude_ref = item("LongitudeRef");
  if (latitude == nullptr || latitude_ref == nullptr || longitude == nullptr ||
      longitude_ref == nullptr) {
    return std::nullopt;
  }
  const auto lat = parse_exif_dms(*latitude, *latitude_ref, kMaxLatitude);
  const auto lon = parse_exif_dms(*longitude, *longitude_ref, kMaxLongitude);
  if (!lat || !lon) {
    return std::nullopt;
  }
  CaptureLocation location{.latitude = *lat, .longitude = *lon};
  // EXIF GPSAltitudeRef: 0 above sea level, 1 below.
  if (const auto* altitude = item("Altitude"); altitude != nullptr) {
    if (const auto meters = parse_unsigned_decimal(*altitude)) {
      const auto* reference = item("AltitudeRef");
      location.altitude_m = reference != nullptr && *reference == "1" ? -*meters : *meters;
    }
  }
  // EXIF GPSMeasureMode: 2 is a two-dimensional fix, 3 three-dimensional.
  if (const auto* mode = item("MeasureMode"); mode != nullptr) {
    if (*mode == "2") {
      location.fix = "2d";
    } else if (*mode == "3") {
      location.fix = "3d";
    }
  }
  const auto* date = item("DateStamp");
  const auto* time = item("TimeStamp");
  if (date != nullptr && time != nullptr) {
    location.fixed_utc = exif_gps_utc(*date, *time);
  }
  return location;
}

}  // namespace

void merge_sony_nonrealtime_meta(CaptureMetadata& capture, std::string_view xml) {
  const std::vector<Tag> tags = scan_tags(xml);
  bool in_gps_group = false;
  std::map<std::string, std::string, std::less<>> gps_items;
  for (const Tag& tag : tags) {
    if (tag.name == "Group") {
      if (tag.closing) {
        in_gps_group = false;
      } else if (const auto* name = attribute(tag, "name")) {
        in_gps_group = *name == kGpsGroup;
      }
      continue;
    }
    if (tag.closing) {
      continue;
    }
    if (tag.name == "Device") {
      if (const auto* make = attribute(tag, "manufacturer")) {
        set_text(capture, capture.make, *make);
      }
      if (const auto* model = attribute(tag, "modelName")) {
        set_text(capture, capture.model, *model);
      }
    } else if (tag.name == "Lens") {
      if (const auto* lens = attribute(tag, "modelName")) {
        set_text(capture, capture.lens_model, *lens);
      }
    } else if (tag.name == "CreationDate" && !capture.recorded) {
      if (const auto* value = attribute(tag, "value")) {
        if (auto recorded = normalized_timestamp(*value)) {
          capture.recorded = std::move(recorded);
          note_source(capture);
        }
      }
    } else if (tag.name == "Item" && in_gps_group) {
      const auto* name = attribute(tag, "name");
      const auto* value = attribute(tag, "value");
      if (name != nullptr && value != nullptr) {
        gps_items[*name] = *value;
      }
    }
  }
  if (!gps_items.empty() && !capture.location) {
    if (auto location = gps_location(gps_items)) {
      capture.location = std::move(location);
      note_source(capture);
    }
  }
}

}  // namespace svp::media
