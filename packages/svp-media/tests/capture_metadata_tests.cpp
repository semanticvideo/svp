// Capture metadata (spec Section 18.3): coordinates and timestamps as cameras
// write them, each container's tags, Sony's NonRealTimeMeta in an ISO BMFF
// `meta` box, and the JSON the builder writes. Every value here is made up.
// With a real ffmpeg and ffprobe ($SVP_FFMPEG / $SVP_FFPROBE or PATH) it also
// reads clips ffmpeg writes with these tags.

#include "svp/media/capture_location.hpp"
#include "svp/media/capture_metadata.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool near(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}

void test_iso6709_forms() {
  const auto degrees = svp::media::parse_iso6709("+12.5000-045.2500+101.5/");
  require(degrees && near(degrees->latitude, 12.5) && near(degrees->longitude, -45.25) &&
              degrees->altitude_m && near(*degrees->altitude_m, 101.5),
          "decimal degrees with altitude");
  const auto minutes = svp::media::parse_iso6709("+1230.00-04515.00/");
  require(minutes && near(minutes->latitude, 12.5) && near(minutes->longitude, -45.25) &&
              !minutes->altitude_m,
          "degrees and minutes");
  const auto seconds = svp::media::parse_iso6709("-123000-0451500/");
  require(seconds && near(seconds->latitude, -12.5) && near(seconds->longitude, -45.25),
          "degrees, minutes and seconds");
  require(!svp::media::parse_iso6709("+95.0-010.0/"), "latitude past 90 is rejected");
  require(!svp::media::parse_iso6709("+10.0-190.0/"), "longitude past 180 is rejected");
  require(!svp::media::parse_iso6709("12.5,-45.25"), "not ISO 6709");
  require(!svp::media::parse_iso6709("+1e1-045.0/"), "exponents aren't digits");
}

void test_exif_dms() {
  const auto north = svp::media::parse_exif_dms("12;30;0.0", "N", 90.0);
  require(north && near(*north, 12.5), "north is positive");
  const auto west = svp::media::parse_exif_dms("45;15;0", "W", 180.0);
  require(west && near(*west, -45.25), "west is negative");
  require(!svp::media::parse_exif_dms("12;30;0", "X", 90.0), "unknown reference");
  require(!svp::media::parse_exif_dms("12;60;0", "N", 90.0), "60 minutes isn't valid");
  require(!svp::media::parse_exif_dms("12;30", "N", 90.0), "three parts are needed");
  require(!svp::media::parse_exif_dms("91;0;0", "S", 90.0), "out of range");
}

nlohmann::json ffprobe_with(nlohmann::json format_tags, std::string format_name = "mov") {
  return {{"format", {{"format_name", std::move(format_name)}, {"tags", std::move(format_tags)}}},
          {"streams", nlohmann::json::array()}};
}

svp::media::CaptureMetadata from_ffprobe(const nlohmann::json& ffprobe) {
  svp::media::CaptureMetadata capture;
  svp::media::merge_ffprobe_json(capture, ffprobe);
  return capture;
}

void test_quicktime_keys() {
  const auto capture = from_ffprobe(ffprobe_with({
      {"com.apple.quicktime.make", "Apple"},
      {"com.apple.quicktime.model", "Test Phone"},
      {"com.apple.quicktime.software", "1.2"},
      {"com.apple.quicktime.creationdate", "2026-01-02T03:04:05-0700"},
      {"com.apple.quicktime.location.ISO6709", "+12.5000-045.2500+101.500/"},
      {"com.apple.quicktime.location.accuracy.horizontal", "4.5"},
      {"creation_time", "2026-01-02T10:04:05.000000Z"},
  }));
  require(capture.make == "Apple" && capture.model == "Test Phone" && capture.software == "1.2",
          "QuickTime make, model, software");
  require(capture.recorded == "2026-01-02T03:04:05-07:00",
          "the camera's local time with its offset wins over the file's UTC time");
  require(capture.location && near(capture.location->latitude, 12.5) &&
              near(capture.location->longitude, -45.25) &&
              capture.location->horizontal_accuracy_m &&
              near(*capture.location->horizontal_accuracy_m, 4.5),
          "QuickTime location and accuracy");
  require(capture.sources == std::vector<std::string>{"quicktime_metadata"}, "sources");
}

void test_mxf_identification() {
  auto ffprobe = ffprobe_with({{"company_name", "MAKER"},
                               {"product_name", "Cam 1"},
                               {"product_version", "1.00"},
                               {"modification_date", "2024-01-02T03:04:05.000000Z"},
                               {"timecode", "01:02:03:04"}},
                              "mxf");
  const auto capture = from_ffprobe(ffprobe);
  require(capture.make == "MAKER" && capture.model == "Cam 1" && capture.software == "1.00",
          "MXF identification");
  require(capture.recorded == "2024-01-02T03:04:05Z", "zero fraction dropped, UTC kept");
  require(capture.start_timecode == "01:02:03:04", "timecode");
  require(capture.sources ==
              std::vector<std::string>{"mxf_identification", "container_tags"},
          "sources in order of first use");
  require(!capture.location, "no location");

  // The same identification tags in an MP4 name the writing software, not a camera.
  ffprobe["format"]["format_name"] = "mov,mp4,m4a,3gp,3g2,mj2";
  const auto mp4 = from_ffprobe(ffprobe);
  require(!mp4.make && !mp4.model, "MXF tags are only read from MXF");
}

void test_container_fallbacks() {
  nlohmann::json ffprobe = ffprobe_with({{"creation_time", "2026-03-04T05:06:07.250000Z"},
                                         {"location", "+12.5000-045.2500/"},
                                         {"timecode", "1:00:00:02"}});
  ffprobe["streams"] = {{{"codec_type", "video"}, {"tags", {{"timecode", "10:00:00/02"}}}},
                        {{"codec_type", "data"}, {"tags", {{"timecode", "10:00:00;02"}}}}};
  const auto capture = from_ffprobe(ffprobe);
  require(capture.recorded == "2026-03-04T05:06:07.25Z", "non-zero fraction kept");
  require(capture.start_timecode == "10:00:00;02",
          "the first stream timecode in the schema's form; others aren't recorded");
  require(capture.location && near(capture.location->latitude, 12.5), "MP4 location tag");

  const auto none = from_ffprobe(
      ffprobe_with({{"creation_time", "2026-03-04 05:06:07"}, {"encoder", "x"}}));
  require(none.empty() && none.sources.empty(),
          "a timestamp without a zone isn't recorded, and other tags aren't capture data");
  require(from_ffprobe(nlohmann::json::object()).empty(),
          "empty ffprobe output");
}

const char* kSonyXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<NonRealTimeMeta xmlns="urn:schemas-professionalDisc:nonRealTimeMeta:ver.2.20">
  <!-- <Device manufacturer="Commented" modelName="Out"/> -->
  <CreationDate value="2026-01-02T03:04:05-08:00"/>
  <Device manufacturer="Sony" modelName="TEST-CAM" serialNo=" 0000001 "/>
  <Lens modelName="Test 24mm F1.4 &amp; more"/>
  <AcquisitionRecord>
    <Group name="ExifGPS">
      <Item name="LatitudeRef" value="N"/>
      <Item name="Latitude" value="12;30;0.000"/>
      <Item name="LongitudeRef" value="W"/>
      <Item name="Longitude" value="45;15;0.000"/>
      <Item name="AltitudeRef" value="1"/>
      <Item name="Altitude" value="3.5"/>
      <Item name="TimeStamp" value="11:04:03.000"/>
      <Item name="DateStamp" value="2026:01:02"/>
      <Item name="Status" value="A"/>
      <Item name="MeasureMode" value="3"/>
      <Item name="MapDatum" value="WGS-84"/>
    </Group>
    <Group name="CameraUnitMetadataSet">
      <Item name="Latitude" value="1;1;1"/>
    </Group>
  </AcquisitionRecord>
</NonRealTimeMeta>)";

void test_sony_nonrealtime_meta() {
  svp::media::CaptureMetadata capture;
  svp::media::merge_sony_nonrealtime_meta(capture, kSonyXml);
  svp::media::merge_ffprobe_json(
      capture, ffprobe_with({{"creation_time", "2026-01-02T11:04:05.000000Z"},
                             {"com.apple.quicktime.location.ISO6709", "+01.0+001.0/"},
                             {"com.apple.quicktime.location.accuracy.horizontal", "5"},
                             {"timecode", "01:00:00:00"}}));
  require(capture.make == "Sony" && capture.model == "TEST-CAM" &&
              capture.serial_number == "0000001",
          "Device, its serial number trimmed");
  require(capture.lens_model == "Test 24mm F1.4 & more", "Lens, entities decoded");
  require(capture.recorded == "2026-01-02T03:04:05-08:00",
          "CreationDate wins over the container's creation time");
  require(capture.location && near(capture.location->latitude, 12.5) &&
              near(capture.location->longitude, -45.25),
          "ExifGPS coordinates, and only from that group");
  require(!capture.location->horizontal_accuracy_m,
          "a later source's accuracy isn't attached to this location");
  require(capture.location->altitude_m && near(*capture.location->altitude_m, -3.5),
          "below sea level");
  require(capture.location->fix == "3d" &&
              capture.location->fixed_utc == "2026-01-02T11:04:03Z",
          "fix and its UTC time");
  require(capture.start_timecode == "01:00:00:00", "container tags fill the rest");
  require(capture.sources ==
              std::vector<std::string>{"sony_nonrealtime_meta", "container_tags"},
          "only sources whose values are kept, in order of first use");
  svp::media::CaptureMetadata dated;
  svp::media::merge_sony_nonrealtime_meta(dated, kSonyXml);
  svp::media::merge_ffprobe_json(
      dated, ffprobe_with({{"creation_time", "2026-01-02T11:04:05.000000Z"}}));
  require(dated.sources == std::vector<std::string>{"sony_nonrealtime_meta"},
          "a container value that isn't kept isn't a source");

  std::string void_fix = kSonyXml;
  void_fix.replace(void_fix.find(R"("Status" value="A")"), 18, R"("Status" value="V")");
  svp::media::CaptureMetadata no_fix;
  svp::media::merge_sony_nonrealtime_meta(no_fix, void_fix);
  require(!no_fix.location && no_fix.model == "TEST-CAM", "a void fix has no location");

  std::string other_datum = kSonyXml;
  other_datum.replace(other_datum.find("WGS-84"), 6, "TOKYO");
  svp::media::CaptureMetadata tokyo;
  svp::media::merge_sony_nonrealtime_meta(tokyo, other_datum);
  require(!tokyo.location, "a datum other than WGS-84 isn't converted");
}

void append_box(std::string& out, const std::string& type, const std::string& payload,
                bool large = false) {
  const auto put32 = [&out](std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
  };
  if (large) {
    put32(1);
    out += type;
    const std::uint64_t size = 16 + payload.size();
    put32(static_cast<std::uint32_t>(size >> 32));
    put32(static_cast<std::uint32_t>(size & 0xFFFFFFFF));
  } else {
    put32(static_cast<std::uint32_t>(8 + payload.size()));
    out += type;
  }
  out += payload;
}

fs::path scratch_dir(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / ("svp-capture-tests-" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void test_sony_xml_in_an_isobmff_meta_box() {
  const fs::path dir = scratch_dir("isobmff");
  const std::string full_box_header(4, '\0');  // version and flags
  std::string meta;
  append_box(meta, "hdlr", full_box_header + std::string(20, '\0'));
  append_box(meta, "xml ", full_box_header + kSonyXml + std::string(1, '\0'));
  std::string file;
  append_box(file, "ftyp", "XAVC");
  append_box(file, "mdat", std::string(64, 'x'), /*large=*/true);
  append_box(file, "meta", full_box_header + meta);
  {
    std::ofstream out(dir / "clip.mp4", std::ios::binary);
    out << file;
  }
  const auto capture =
      svp::media::read_capture_metadata(dir / "clip.mp4", dir / "no-such-ffprobe");
  require(capture.model == "TEST-CAM" && capture.location,
          "the meta box's XML is read even when ffprobe can't run");

  std::string not_isobmff = file;
  not_isobmff.replace(4, 4, "free");
  {
    std::ofstream out(dir / "other.bin", std::ios::binary);
    out << not_isobmff;
  }
  require(svp::media::read_capture_metadata(dir / "other.bin", dir / "no-such-ffprobe").empty(),
          "a file that doesn't start with ftyp isn't walked");
  require(svp::media::read_capture_metadata(dir / "missing.mp4", dir / "no-such-ffprobe").empty(),
          "a missing file has no capture metadata");
  fs::remove_all(dir);
}

void test_json() {
  svp::media::CaptureMetadata capture;
  require(capture.empty(), "nothing set");
  capture.model = "Cam";
  capture.start_timecode = "01:00:00:00";
  capture.sources = {"container_tags"};
  const auto json = svp::media::capture_metadata_to_json(capture);
  require(json == nlohmann::json{{"schema_version", "svp-capture-v1"},
                                 {"camera", {{"model", "Cam"}}},
                                 {"start_timecode", "01:00:00:00"},
                                 {"sources", {"container_tags"}}},
          "only what's set: " + json.dump());
  svp::media::CaptureMetadata serial;
  serial.serial_number = "0000001";
  require(!serial.empty() && svp::media::capture_metadata_to_json(serial)["camera"] ==
                                 nlohmann::json{{"serial_number", "0000001"}},
          "a serial number alone is capture metadata");
}

bool rejected(const nlohmann::json& document) {
  try {
    (void)svp::media::capture_metadata_from_json(document);
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}

void test_json_read_back() {
  svp::media::CaptureMetadata capture;
  svp::media::merge_sony_nonrealtime_meta(capture, kSonyXml);
  svp::media::merge_ffprobe_json(capture, ffprobe_with({{"com.apple.quicktime.software", "2.0"},
                                                        {"timecode", "01:00:00;00"}}));
  const nlohmann::json json = svp::media::capture_metadata_to_json(capture);
  require(svp::media::capture_metadata_to_json(svp::media::capture_metadata_from_json(json)) ==
              json,
          "every member reads back: " + json.dump());
  const svp::media::CaptureMetadata none = svp::media::capture_metadata_from_json(
      svp::media::capture_metadata_to_json(svp::media::CaptureMetadata{}));
  require(none.empty() && none.sources.empty(), "a source with none reads back as none");

  const auto with = [&json](const std::string& pointer, const nlohmann::json& value) {
    nlohmann::json changed = json;
    changed[nlohmann::json::json_pointer(pointer)] = value;
    return changed;
  };
  require(rejected(nlohmann::json::array()), "not an object");
  require(rejected(with("/schema_version", "svp-capture-v2")), "another schema version");
  require(rejected(with("/schema_version", 1)) && rejected(with("/schema_version", nullptr)),
          "a schema version that isn't text");
  require(rejected(with("/note", "x")), "an unknown member");
  require(rejected(with("/camera/lens", "x")), "an unknown camera member");
  require(rejected(with("/camera/serial_number", " 1")), "untrimmed text");
  require(rejected(with("/camera/model", 1)), "a number for text");
  require(rejected(with("/recorded", "2026-01-02T03:04:05")), "a time with no offset");
  require(rejected(with("/start_timecode", "1:00:00:00")), "not a timecode");
  require(rejected(with("/location/latitude", 91)), "latitude out of range");
  require(rejected(with("/location/fix", "1d")), "an unknown fix");
  require(rejected(with("/location/fixed_utc", "2026-01-02T11:04:03-08:00")),
          "a fix time not in UTC");
  require(rejected(with("/sources/0", "exif")), "an unknown source");
  nlohmann::json no_sources = json;
  no_sources.erase("sources");
  require(rejected(no_sources), "sources missing");
}

struct Tools {
  std::string ffmpeg;
  std::string ffprobe;
};

std::optional<Tools> real_tools() {
  const char* ffmpeg = std::getenv("SVP_FFMPEG");
  const char* ffprobe = std::getenv("SVP_FFPROBE");
  Tools tools{ffmpeg != nullptr ? ffmpeg : "ffmpeg", ffprobe != nullptr ? ffprobe : "ffprobe"};
  for (const auto& tool : {tools.ffmpeg, tools.ffprobe}) {
    if (std::system(("\"" + tool + "\" -version > /dev/null 2>&1").c_str()) != 0) {
      return std::nullopt;
    }
  }
  return tools;
}

void run_ffmpeg(const Tools& tools, const std::string& arguments) {
  const std::string command = "\"" + tools.ffmpeg + "\" -nostdin -loglevel error -y " +
                              "-f lavfi -i testsrc2=duration=1:size=160x120:rate=25 " +
                              arguments;
  require(std::system(command.c_str()) == 0, "ffmpeg: " + command);
}

void test_clips_ffmpeg_writes(const Tools& tools) {
  const fs::path dir = scratch_dir("ffmpeg");
  const fs::path mp4 = dir / "tagged.mp4";
  run_ffmpeg(tools, "-metadata creation_time=2026-01-02T03:04:05Z "
                    "-metadata location=+12.5000-045.2500/ -timecode 01:02:03:04 \"" +
                        mp4.string() + "\"");
  const auto tagged = svp::media::read_capture_metadata(mp4, tools.ffprobe);
  require(tagged.recorded == "2026-01-02T03:04:05Z", "creation_time");
  require(tagged.location && near(tagged.location->latitude, 12.5) &&
              near(tagged.location->longitude, -45.25),
          "MP4 location");
  require(tagged.start_timecode == "01:02:03:04", "timecode track");

  const fs::path mov = dir / "keys.mov";
  run_ffmpeg(tools, "-movflags use_metadata_tags "
                    "-metadata com.apple.quicktime.make=Apple "
                    "-metadata com.apple.quicktime.model=Test \"" +
                        mov.string() + "\"");
  const auto keys = svp::media::read_capture_metadata(mov, tools.ffprobe);
  require(keys.make == "Apple" && keys.model == "Test", "QuickTime keys");

  const fs::path plain = dir / "plain.mp4";
  run_ffmpeg(tools, "-map_metadata -1 -fflags +bitexact \"" + plain.string() + "\"");
  require(svp::media::read_capture_metadata(plain, tools.ffprobe).empty(),
          "a clip with no capture tags has none");
  fs::remove_all(dir);
}

}  // namespace

int main() {
  try {
    test_iso6709_forms();
    test_exif_dms();
    test_quicktime_keys();
    test_mxf_identification();
    test_container_fallbacks();
    test_sony_nonrealtime_meta();
    test_sony_xml_in_an_isobmff_meta_box();
    test_json();
    test_json_read_back();
    if (const auto tools = real_tools()) {
      test_clips_ffmpeg_writes(*tools);
    } else {
      std::cout << "Skipping: clips written by ffmpeg need ffmpeg and ffprobe "
                   "($SVP_FFMPEG/$SVP_FFPROBE or PATH)\n";
    }
  } catch (const std::exception& error) {
    std::cerr << "capture metadata tests failed: " << error.what() << "\n";
    return 1;
  }
  std::cout << "capture metadata tests passed\n";
  return 0;
}
