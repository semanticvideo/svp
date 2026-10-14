#include "resize/resize_encoder.hpp"

#include <algorithm>
#include <cctype>

namespace svp::builder::resize {

ResizeContainer resize_container_for(const std::filesystem::path& source) {
  std::string extension = source.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".mp4" || extension == ".m4v" ? ResizeContainer::mp4
                                                    : ResizeContainer::quicktime;
}

std::filesystem::path resized_media_path(const std::filesystem::path& output,
                                         const ResizeGeometry& geometry,
                                         ResizeContainer container) {
  const int short_side = std::min(geometry.display_width, geometry.display_height);
  std::filesystem::path path = output.parent_path() / output.stem();
  path += "." + std::to_string(short_side) + "p";
  path += container == ResizeContainer::mp4 ? ".mp4" : ".mov";
  return path;
}

std::string resize_encoder_time_base(const svp::media::Rational& source_time_base) {
  const svp::media::Rational time_base = svp::media::is_valid(source_time_base)
                                             ? svp::media::normalize(source_time_base)
                                             : svp::media::Rational{0, 0};
  if (time_base.numerator > 0 && time_base.denominator > 0 &&
      time_base.denominator <= kMpeg4MaxTimeBaseDenominator) {
    return "demux";
  }
  return std::string(kFineTimeBase);
}

std::vector<std::string> resize_ffmpeg_command(const std::filesystem::path& ffmpeg_path,
                                               const std::filesystem::path& source,
                                               const svp::media::VideoStreamProbe& video,
                                               const ResizeGeometry& geometry,
                                               ResizeContainer container,
                                               const std::filesystem::path& output) {
  return {ffmpeg_path.string(),
          "-nostdin",
          "-hide_banner",
          "-loglevel",
          "error",
          "-noautorotate",
          "-i",
          source.string(),
          "-map",
          "0:" + std::to_string(video.index),
          "-map",
          "0:a?",
          "-c:a",
          "copy",
          "-vf",
          "scale=" + std::to_string(geometry.stored_width) + ":" +
              std::to_string(geometry.stored_height),
          "-c:v",
          "mpeg4",
          "-q:v",
          std::string(kMpeg4Quantizer),
          "-threads",
          std::string(kEncoderThreads),
          "-fps_mode",
          "passthrough",
          "-enc_time_base",
          resize_encoder_time_base(video.timing.timebase),
          "-f",
          container == ResizeContainer::mp4 ? "mp4" : "mov",
          "-y",
          output.string()};
}

}  // namespace svp::builder::resize
