#include "svp/media/media_probe.hpp"

#include "svp/media/canonical_timing.hpp"

#include <fstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>

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

std::optional<std::int64_t> optional_int64_or_string(const nlohmann::json& value,
                                                     std::string_view property,
                                                     std::string_view source_name) {
  const auto found = value.find(property);
  if (found == value.end() || found->is_null()) {
    return std::nullopt;
  }
  if (found->is_number_integer()) {
    return found->get<std::int64_t>();
  }
  if (!found->is_string()) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be an integer or integer string");
  }

  try {
    std::size_t consumed = 0;
    const std::string raw = found->get<std::string>();
    const std::int64_t parsed = std::stoll(raw, &consumed);
    if (consumed != raw.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed;
  } catch (const std::exception&) {
    throw std::runtime_error(std::string(source_name) + "." + std::string(property) +
                             " must be an integer or integer string");
  }
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

std::string optional_rational_string_or(const nlohmann::json& value,
                                        std::string_view property,
                                        std::string fallback,
                                        std::string_view source_name) {
  const std::optional<std::string> raw = optional_string(value, property, source_name);
  if (!raw.has_value() || !parse_rational(*raw).has_value()) {
    return fallback;
  }
  return *raw;
}

std::optional<Rational> optional_valid_rational_property(
    const nlohmann::json& value,
    std::string_view property,
    std::string_view source_name) {
  const std::optional<std::string> raw = optional_string(value, property, source_name);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return parse_rational(*raw);
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

StreamTiming parse_nested_or_flat_stream_timing(const nlohmann::json& value,
                                                std::string_view source_name) {
  const auto found = value.find("timing");
  if (found == value.end() || found->is_null()) {
    return parse_stream_timing(value, source_name);
  }
  if (!found->is_object()) {
    throw std::runtime_error(std::string(source_name) + ".timing must be an object");
  }
  return parse_stream_timing(*found, std::string(source_name) + ".timing");
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

std::string shell_quote(const std::filesystem::path& path) {
  std::string quoted = "'";
  for (const char character : path.string()) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
}

std::string read_command_output(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    throw std::runtime_error("unable to run ffprobe");
  }

  std::string output;
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }

  const int status = pclose(pipe);
  if (status == -1) {
    throw std::runtime_error("unable to close ffprobe process");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("ffprobe failed with exit status " +
                             std::to_string(WIFEXITED(status) ? WEXITSTATUS(status)
                                                              : status));
  }
  return output;
}

std::int32_t parse_rotation_degrees(const nlohmann::json& stream) {
  const std::optional<std::string> rotate_tag =
      optional_string(stream, "rotate", "stream");
  if (rotate_tag.has_value()) {
    try {
      return static_cast<std::int32_t>(std::stol(*rotate_tag));
    } catch (const std::exception&) {
      return 0;
    }
  }

  const auto tags = stream.find("tags");
  if (tags != stream.end() && tags->is_object()) {
    const std::optional<std::string> rotate =
        optional_string(*tags, "rotate", "tags");
    if (rotate.has_value()) {
      try {
        return static_cast<std::int32_t>(std::stol(*rotate));
      } catch (const std::exception&) {
        return 0;
      }
    }
  }

  const auto side_data_list = stream.find("side_data_list");
  if (side_data_list != stream.end() && side_data_list->is_array()) {
    for (const nlohmann::json& side_data : *side_data_list) {
      if (!side_data.is_object()) {
        continue;
      }
      const auto rotation = side_data.find("rotation");
      if (rotation == side_data.end() || rotation->is_null()) {
        continue;
      }
      try {
        if (rotation->is_number_integer()) {
          return rotation->get<std::int32_t>();
        }
        if (rotation->is_number_float()) {
          return static_cast<std::int32_t>(rotation->get<double>());
        }
        if (rotation->is_string()) {
          return static_cast<std::int32_t>(std::stol(rotation->get<std::string>()));
        }
      } catch (const std::exception&) {
        return 0;
      }
    }
  }

  return 0;
}

std::optional<std::int64_t> parse_decimal_seconds_to_microseconds(
    const std::string& raw) {
  const std::size_t dot = raw.find('.');
  const std::string whole = dot == std::string::npos ? raw : raw.substr(0, dot);
  std::string fraction = dot == std::string::npos ? "" : raw.substr(dot + 1);
  if (whole.empty() || whole[0] == '-') {
    return std::nullopt;
  }
  while (fraction.size() < 6) {
    fraction.push_back('0');
  }
  const bool round_up = fraction.size() > 6 && fraction[6] >= '5';
  fraction.resize(6);
  try {
    return (std::stoll(whole) * 1000000) + std::stoll(fraction) +
           (round_up ? 1 : 0);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

StreamTiming parse_ffprobe_timing(const nlohmann::json& stream,
                                  std::string_view source_name) {
  StreamTiming timing;
  timing.timebase = optional_rational_property(stream, "time_base", source_name)
                        .value_or(Rational{0, 1});
  timing.start_pts =
      optional_int64_or_string(stream, "start_pts", source_name).value_or(0);
  timing.duration_pts = optional_int64_or_string(stream, "duration_ts", source_name);
  timing.average_frame_rate =
      optional_valid_rational_property(stream, "avg_frame_rate", source_name);
  timing.frame_count = optional_int64_or_string(stream, "nb_frames", source_name);
  return timing;
}

nlohmann::json ffprobe_json_to_probe_contract(const nlohmann::json& ffprobe,
                                              std::string_view source_name) {
  if (!ffprobe.is_object()) {
    throw std::runtime_error(std::string(source_name) + " ffprobe output must be an object");
  }

  nlohmann::json contract = {
      {"format_name", ""},
      {"video_streams", nlohmann::json::array()},
      {"audio_streams", nlohmann::json::array()},
  };

  const auto format = ffprobe.find("format");
  if (format != ffprobe.end() && format->is_object()) {
    contract["format_name"] =
        optional_string(*format, "format_name", "format").value_or("");
    const std::optional<std::string> duration =
        optional_string(*format, "duration", "format");
    if (duration.has_value()) {
      const std::optional<std::int64_t> duration_us =
          parse_decimal_seconds_to_microseconds(*duration);
      if (duration_us.has_value()) {
        contract["container_timing"] = {
            {"timebase", "1/1000000"},
            {"start_pts", 0},
            {"duration_pts", *duration_us},
        };
      }
    }
  }

  const auto streams = ffprobe.find("streams");
  if (streams == ffprobe.end() || !streams->is_array()) {
    throw std::runtime_error(std::string(source_name) +
                             " ffprobe output must contain streams array");
  }

  std::size_t video_ordinal = 1;
  std::size_t audio_ordinal = 1;
  for (const nlohmann::json& stream : *streams) {
    if (!stream.is_object()) {
      continue;
    }
    const std::string codec_type =
        optional_string(stream, "codec_type", "stream").value_or("");
    if (codec_type == "video") {
      const StreamTiming timing = parse_ffprobe_timing(stream, "video stream");
      contract["video_streams"].push_back({
          {"id", generated_id("vstream", video_ordinal++)},
          {"index", optional_int32(stream, "index", 0, "video stream")},
          {"codec_name", optional_string(stream, "codec_name", "video stream").value_or("")},
          {"width", require_int32(stream, "width", "video stream")},
          {"height", require_int32(stream, "height", "video stream")},
          {"rotation_degrees", parse_rotation_degrees(stream)},
          {"pixel_aspect_ratio",
           optional_rational_string_or(stream, "sample_aspect_ratio", "1:1",
                                       "video stream")},
          {"timing", stream_timing_to_json(timing)},
      });
    } else if (codec_type == "audio") {
      const StreamTiming timing = parse_ffprobe_timing(stream, "audio stream");
      const std::optional<std::string> sample_rate =
          optional_string(stream, "sample_rate", "audio stream");
      contract["audio_streams"].push_back({
          {"id", generated_id("astream", audio_ordinal++)},
          {"index", optional_int32(stream, "index", 0, "audio stream")},
          {"codec_name", optional_string(stream, "codec_name", "audio stream").value_or("")},
          {"sample_rate", sample_rate.has_value() ? std::stoi(*sample_rate) : 0},
          {"channels", optional_int32(stream, "channels", 0, "audio stream")},
          {"timing", stream_timing_to_json(timing)},
      });
    }
  }

  return contract;
}

}  // namespace

MediaProbe parse_media_probe_json(const nlohmann::json& value,
                                  std::string_view source_name) {
  if (!value.is_object()) {
    throw std::runtime_error(std::string(source_name) + " must be an object");
  }

  MediaProbe probe;
  probe.format_name = optional_string(value, "format_name", source_name).value_or("");
  if (value.contains("container_timing")) {
    const auto& timing = require_property(value, "container_timing", source_name);
    if (!timing.is_object()) {
      throw std::runtime_error(std::string(source_name) +
                               ".container_timing must be an object");
    }
    probe.container_timing = parse_stream_timing(timing,
                                                std::string(source_name) +
                                                    ".container_timing");
  } else if (value.contains("timebase") || value.contains("duration_pts")) {
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
    const auto rotation = stream.find("rotation_degrees");
    if (rotation != stream.end() && !rotation->is_null()) {
      if (!rotation->is_number_integer()) {
        throw std::runtime_error(stream_source + ".rotation_degrees must be an integer");
      }
      parsed.rotation_degrees = rotation->get<std::int32_t>();
    } else {
      parsed.rotation_degrees = parse_rotation_degrees(stream);
    }
    parsed.pixel_aspect_ratio =
        optional_rational_property(stream, "pixel_aspect_ratio", stream_source)
            .value_or(Rational{1, 1});
    parsed.timing = parse_nested_or_flat_stream_timing(stream, stream_source);
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
    parsed.timing = parse_nested_or_flat_stream_timing(stream, stream_source);
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

MediaProbe probe_media_with_ffprobe(const std::filesystem::path& source_path,
                                    const std::filesystem::path& ffprobe_path) {
  const std::string command = shell_quote(ffprobe_path) +
                              " -v error -print_format json -show_format "
                              "-show_streams " +
                              shell_quote(source_path);
  const std::string output = read_command_output(command);
  const nlohmann::json ffprobe_output = nlohmann::json::parse(output);
  return parse_media_probe_json(ffprobe_json_to_probe_contract(ffprobe_output,
                                                               source_path.string()),
                                source_path.string());
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
