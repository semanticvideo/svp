#pragma once

#include "svp/media/rational.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::media {

struct StreamTiming {
  Rational timebase{0, 1};
  std::int64_t start_pts = 0;
  std::optional<std::int64_t> duration_pts;
  std::optional<Rational> average_frame_rate;
  std::optional<std::int64_t> frame_count;
};

struct VideoStreamProbe {
  std::string id;
  std::int32_t index = 0;
  std::string codec_name;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t rotation_degrees = 0;
  Rational pixel_aspect_ratio{1, 1};
  StreamTiming timing;
};

struct AudioStreamProbe {
  std::string id;
  std::int32_t index = 0;
  std::string codec_name;
  std::int32_t sample_rate = 0;
  std::int32_t channels = 0;
  StreamTiming timing;
};

struct MediaProbe {
  std::string format_name;
  std::optional<StreamTiming> container_timing;
  std::vector<VideoStreamProbe> video_streams;
  std::vector<AudioStreamProbe> audio_streams;
};

[[nodiscard]] MediaProbe parse_media_probe_json(const nlohmann::json& value,
                                                std::string_view source_name);
[[nodiscard]] MediaProbe load_media_probe_json(const std::filesystem::path& path);
[[nodiscard]] nlohmann::json media_probe_to_json(const MediaProbe& probe);

}  // namespace svp::media
