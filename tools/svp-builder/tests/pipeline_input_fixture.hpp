#pragma once

// Minimal synthetic pipeline inputs: a probe JSON describing a short
// audio+video clip and placeholder media bytes. Used with ffmpeg=/usr/bin/true
// and a synthetic model cache so package-stage builds run without real media.

#include <filesystem>
#include <fstream>

namespace svp::builder::test {

inline std::filesystem::path write_minimal_probe_json(
    const std::filesystem::path& dir) {
  const std::filesystem::path probe_path = dir / "probe.json";
  std::ofstream out(probe_path);
  out << R"({
    "format_name": "mov,mp4,m4a,3gp,3g2,mj2",
    "container_timing": {
      "timebase": "1/1000000",
      "start_pts": 0,
      "duration_pts": 3000000
    },
    "video_streams": [
      {
        "id": "vstream_0001",
        "index": 0,
        "codec_name": "h264",
        "width": 1920,
        "height": 1080,
        "pixel_aspect_ratio": "1:1",
        "timing": {
          "timebase": "1/30000",
          "start_pts": 0,
          "duration_pts": 90090,
          "avg_frame_rate": "30000/1001",
          "frame_count": 90
        }
      }
    ],
    "audio_streams": [
      {
        "id": "astream_0001",
        "index": 1,
        "codec_name": "aac",
        "sample_rate": 48000,
        "channels": 2,
        "timing": {
          "timebase": "1/48000",
          "start_pts": 0,
          "duration_pts": 144000
        }
      }
    ]
  })";
  out.close();
  return probe_path;
}

inline std::filesystem::path write_mock_media_file(
    const std::filesystem::path& dir) {
  const std::filesystem::path media_path = dir / "test_video.mp4";
  std::ofstream out(media_path, std::ios::binary);
  out << "mock media bytes";
  out.close();
  return media_path;
}

}  // namespace svp::builder::test
