#pragma once

// How the resized media of --max-resolution is written (resized_source.hpp):
// its file name and container, and the ffmpeg command.
//
// One encoder on every platform: MPEG-4 Part 2 in software, which every
// ffmpeg build has, including the LGPL runtime bundle. No frame is added or
// dropped (-fps_mode passthrough); the encoder keeps the source stream's time
// base when MPEG-4 Part 2 can store it and otherwise uses a finer-than-a-frame
// one it can (resize_encoder_time_base); every audio stream is copied without
// re-encoding. The source's rotation metadata is kept rather than applied
// (-noautorotate). The encoder runs on a fixed number of threads, because its
// slices follow its thread count: the same source and ffmpeg give the same
// bytes on every machine.

#include "resize/resize_geometry.hpp"

#include "svp/media/media_probe.hpp"
#include "svp/media/rational.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::resize {

// MPEG-4 Part 2 quantizer (-q:v, 1 best..31). Measured on a 60 s 3840x2160
// H.264 slice of the 17-minute RV Park video on an M4 Mac: q 2 wrote an 82 MB
// 1080p file from the 375 MB slice in 11 s, and a distributed build of it
// lost none of the 4K build's OCR numbers.
inline constexpr std::string_view kMpeg4Quantizer = "2";

// Encoder threads (slices), the same on every machine.
inline constexpr std::string_view kEncoderThreads = "8";

// MPEG-4 Part 2 stores timestamps in a time base whose denominator is at most
// 65535 (vop_time_increment_resolution has 16 bits).
inline constexpr int kMpeg4MaxTimeBaseDenominator = 65535;

// The time base used when the source's is finer than MPEG-4 Part 2 can store
// (many Android phones write 1/90000 s): 1/60000 s. Each frame keeps its
// source time rounded to it, at most 9 microseconds off, far less than a
// frame; the resized media's timing is its own and the build describes it.
inline constexpr std::string_view kFineTimeBase = "1/60000";

enum class ResizeContainer { mp4, quicktime };

// MP4 for an MP4 source (.mp4, .m4v); QuickTime for every other, which takes
// the audio codecs of camera and phone files (AAC, PCM, ALAC) by stream copy.
// Both are ISO base media files.
[[nodiscard]] ResizeContainer resize_container_for(const std::filesystem::path& source);

// The resized media's path: beside `output` (the --out artifact), named after
// it, with the resized short side: `talk.svpi` at 1080 -> `talk.1080p.mp4`.
[[nodiscard]] std::filesystem::path resized_media_path(const std::filesystem::path& output,
                                                       const ResizeGeometry& geometry,
                                                       ResizeContainer container);

// The encoder time base: the source stream's ("demux") when MPEG-4 Part 2 can
// store it, otherwise kFineTimeBase.
[[nodiscard]] std::string resize_encoder_time_base(const svp::media::Rational& source_time_base);

// The ffmpeg command that writes `source`'s primary video stream `video`,
// resized to `geometry`, and every audio stream, to `output`.
[[nodiscard]] std::vector<std::string> resize_ffmpeg_command(
    const std::filesystem::path& ffmpeg_path, const std::filesystem::path& source,
    const svp::media::VideoStreamProbe& video, const ResizeGeometry& geometry,
    ResizeContainer container, const std::filesystem::path& output);

}  // namespace svp::builder::resize
