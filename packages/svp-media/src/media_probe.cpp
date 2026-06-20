#include "svp/media/media_probe.hpp"

#include "svp/media/canonical_timing.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace svp::media {
namespace {

const nlohmann::json& require_property(const nlohmann::json& value,
                                       std::string_view property,
                                       std::string_view source_name) {
  const auto found = value.find(property);
  if (found == value.end()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " is required");
  }
  return *found;
}

std::string require_string(const nlohmann::json& value,
                           std::string_view property,
                           std::string_view source_name) {
  const auto& property_value = require_property(value, property, source_name);
  if (!property_value.is_string()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be a string");
  }
  return property_value.get<std::string>();
}

std::optional<std::string> optional_string(const nlohmann::json& value,
                                           std::string_view property,
                                           std::string_view source_name) {
  const auto found = value.find(property);
  if (found == value.end() || found->is_null()) {
    return std::nullopt;
  }
  if (!found->is_string()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be a string");
  }
  return found->get<std::string>();
}

std::int32_t require_int32(const nlohmann::json& value,
                           std::string_view property,
                           std::string_view source_name) {
  const auto& property_value = require_property(value, property, source_name);
  if (!property_value.is_number_integer()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be an integer");
  }
  return property_value.get<std::int32_t>();
}

std::int32_t optional_int32(const nlohmann::json& value,
                            std::string_view property,
                            std::int32_t fallback,
                            std::string_view source_name) {
  const auto found = value.find(property);
  if (found == value.end() || found->is_null()) {
    return fallback;
  }
  if (!found->is_number_integer()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be an integer");
  }
  return found->get<std::int32_t>();
}

std::optional<std::int64_t> optional_int64(const nlohmann::json& value,
                                           std::string_view property,
                                           std::string_view source_name) {
  const auto found = value.find(property);
  if (found == value.end() || found->is_null()) {
    return std::nullopt;
  }
  if (!found->is_number_integer()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be an integer");
  }
  return found->get<std::int64_t>();
}

std::optional<Rational> optional_rational_property(const nlohmann::json& value,
                                                   std::string_view property,
                                                   std::string_view source_name) {
  const std::optional<std::string> raw = optional_string(value, property, source_name);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  const std::optional<Rational> parsed = parse_rational(*raw);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be a rational string");
  }
  return *parsed;
}

StreamTiming parse_stream_timing(const nlohmann::json& value,
                                 std::string_view source_name) {
  StreamTiming timing;
  timing.timebase = optional_rational_property(value, "timebase", source_name)
                        .value_or(Rational{0, 1});
  timing.start_pts = optional_int64(value, "start_pts", source_name).value_or(0);
  timing.duration_pts = optional_int64(value, "duration_pts", source_name);
  timing.average_frame_rate =
      optional_rational_property(value, "avg_frame_rate", source_name);
  timing.frame_count = optional_int64(value, "frame_count", source_name);
  return timing;
}

std::string generated_id(std::string_view prefix, std::size_t ordinal) {
  std::ostringstream stream;
  stream << prefix << "_" << std::setw(4) << std::setfill('0') << ordinal;
  return stream.str();
}

nlohmann::json stream_timing_to_json(const StreamTiming& timing) {
  nlohmann::json value = nlohmann::json::object();
  if (is_valid(timing.timebase) && timing.timebase.numerator != 0) {
    value["timebase"] = format_rational(timing.timebase);
  }
  value["start_pts"] = timing.start_pts;
  if (timing.duration_pts.has_value()) {
    value["duration_pts"] = *timing.duration_pts;
    if (is_valid(timing.timebase) && timing.timebase.numerator > 0) {
      value["duration_us"] = pts_to_microseconds(*timing.duration_pts, timing.timebase);
    }
  }
  if (timing.average_frame_rate.has_value()) {
    value["avg_frame_rate"] = format_rational(*timing.average_frame_rate);
  }
  if (timing.frame_count.has_value()) {
    value["frame_count"] = *timing.frame_count;
  }
  return value;
}

}  // namespace

MediaProbe parse_media_probe_json(const nlohmann::json& value,
                                  std::string_view source_name) {
  if (!value.is_object()) {
    throw std::runtime_error(std::string(source_name) + " must be an object");
  }

  MediaProbe probe;
  probe.format_name = optional_string(value, "format_name", source_name).value_or("");
  if (value.contains("timebase") || value.contains("duration_pts")) {
    probe.container_timing = parse_stream_timing(value, source_name);
  }

  const auto& video_streams = require_property(value, "video_streams", source_name);
  if (!video_streams.is_array()) {
    throw std::runtime_error(std::string(source_name) + ".video_streams must be an array");
  }

  for (std::size_t index = 0; index < video_streams.size(); ++index) {
    const auto& stream = video_streams[index];
    const std::string stream_source =
        std::string(source_name) + ".video_streams[" + std::to_string(index) + "]";
    if (!stream.is_object()) {
      throw std::runtime_error(stream_source + " must be an object");
    }
    VideoStreamProbe parsed;
    parsed.id = optional_string(stream, "id", stream_source).value_or(
        generated_id("vstream", index + 1));
    parsed.index = optional_int32(stream, "index", static_cast<std::int32_t>(index),
                                  stream_source);
    parsed.codec_name = optional_string(stream, "codec_name", stream_source).value_or("");
    parsed.width = require_int32(stream, "width", stream_source);
    parsed.height = require_int32(stream, "height", stream_source);
    parsed.rotation_degrees = optional_int32(stream, "rotation_degrees", 0, stream_source);
    parsed.pixel_aspect_ratio =
        optional_rational_property(stream, "pixel_aspect_ratio", stream_source)
            .value_or(Rational{1, 1});
    parsed.timing = parse_stream_timing(stream, stream_source);
    probe.video_streams.push_back(std::move(parsed));
  }

  const auto found_audio = value.find("audio_streams");
  if (found_audio == value.end() || found_audio->is_null()) {
    return probe;
  }
  if (!found_audio->is_array()) {
    throw std::runtime_error(std::string(source_name) + ".audio_streams must be an array");
  }

  for (std::size_t index = 0; index < found_audio->size(); ++index) {
    const auto& stream = (*found_audio)[index];
    const std::string stream_source =
        std::string(source_name) + ".audio_streams[" + std::to_string(index) + "]";
    if (!stream.is_object()) {
      throw std::runtime_error(stream_source + " must be an object");
    }
    AudioStreamProbe parsed;
    parsed.id = optional_string(stream, "id", stream_source).value_or(
        generated_id("astream", index + 1));
    parsed.index = optional_int32(stream, "index", static_cast<std::int32_t>(index),
                                  stream_source);
    parsed.codec_name = optional_string(stream, "codec_name", stream_source).value_or("");
    parsed.sample_rate = optional_int32(stream, "sample_rate", 0, stream_source);
    parsed.channels = optional_int32(stream, "channels", 0, stream_source);
    parsed.timing = parse_stream_timing(stream, stream_source);
    probe.audio_streams.push_back(std::move(parsed));
  }

  return probe;
}

MediaProbe load_media_probe_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open media probe JSON: " + path.string());
  }

  nlohmann::json value;
  input >> value;
  return parse_media_probe_json(value, path.string());
}

nlohmann::json media_probe_to_json(const MediaProbe& probe) {
  nlohmann::json value = {
      {"format_name", probe.format_name},
      {"video_streams", nlohmann::json::array()},
      {"audio_streams", nlohmann::json::array()},
  };

  if (probe.container_timing.has_value()) {
    value["container_timing"] = stream_timing_to_json(*probe.container_timing);
  }

  for (const VideoStreamProbe& stream : probe.video_streams) {
    value["video_streams"].push_back({
        {"id", stream.id},
        {"index", stream.index},
        {"codec_name", stream.codec_name},
        {"width", stream.width},
        {"height", stream.height},
        {"rotation_degrees", stream.rotation_degrees},
        {"pixel_aspect_ratio", format_rational(stream.pixel_aspect_ratio)},
        {"timing", stream_timing_to_json(stream.timing)},
    });
  }

  for (const AudioStreamProbe& stream : probe.audio_streams) {
    value["audio_streams"].push_back({
        {"id", stream.id},
        {"index", stream.index},
        {"codec_name", stream.codec_name},
        {"sample_rate", stream.sample_rate},
        {"channels", stream.channels},
        {"timing", stream_timing_to_json(stream.timing)},
    });
  }

  return value;
}

}  // namespace svp::media
