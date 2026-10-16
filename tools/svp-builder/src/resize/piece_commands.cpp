#include "resize/piece_commands.hpp"

#include <cstdlib>
#include <stdexcept>

namespace svp::builder::resize {
namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

// "S.ffffff": ffmpeg reads a time in seconds; microseconds keep it exact.
std::string seconds_text(std::int64_t microseconds) {
  const bool negative = microseconds < 0;
  const std::int64_t magnitude = negative ? -microseconds : microseconds;
  std::string fraction = std::to_string(magnitude % kMicrosecondsPerSecond);
  fraction.insert(0, 6 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(magnitude / kMicrosecondsPerSecond) + "." +
         fraction;
}

}  // namespace

PieceEncoding piece_encoding(const ResizeGeometry& geometry,
                             const svp::media::Rational& source_time_base) {
  // resize_encoder_time_base names the source's as "demux"; a piece's
  // container may store another, so a piece names it outright.
  const std::string chosen = resize_encoder_time_base(source_time_base);
  return PieceEncoding{
      .stored_width = geometry.stored_width,
      .stored_height = geometry.stored_height,
      .time_base = chosen == "demux"
                       ? svp::media::format_rational(svp::media::normalize(source_time_base))
                       : chosen};
}

std::vector<std::string> piece_cut_command(const std::filesystem::path& ffmpeg_path,
                                           const std::filesystem::path& source,
                                           std::int32_t video_index,
                                           const std::vector<std::size_t>& cuts,
                                           const PieceContainer& container,
                                           const std::filesystem::path& pattern) {
  if (cuts.size() < 2 || cuts.front() != 0) {
    throw std::invalid_argument("a piece cut needs at least two pieces, the first at packet 0");
  }
  std::string frames;
  for (std::size_t i = 1; i < cuts.size(); ++i) {
    frames += (i > 1 ? "," : "") + std::to_string(cuts[i]);
  }
  return {ffmpeg_path.string(),
          "-nostdin",
          "-hide_banner",
          "-loglevel",
          "error",
          "-copyts",
          "-i",
          source.string(),
          "-map",
          "0:" + std::to_string(video_index),
          "-c",
          "copy",
          "-avoid_negative_ts",
          "disabled",
          "-f",
          "segment",
          "-segment_format",
          std::string(container.muxer),
          "-segment_frames",
          frames,
          "-reset_timestamps",
          "0",
          "-y",
          pattern.string()};
}

std::vector<std::string> piece_encode_command(const std::filesystem::path& ffmpeg_path,
                                              const std::filesystem::path& piece,
                                              const PieceEncoding& encoding,
                                              const std::filesystem::path& output) {
  return {ffmpeg_path.string(),
          "-nostdin",
          "-hide_banner",
          "-loglevel",
          "error",
          "-copyts",
          "-noautorotate",
          "-i",
          piece.string(),
          "-map",
          "0:v:0",
          "-vf",
          "scale=" + std::to_string(encoding.stored_width) + ":" +
              std::to_string(encoding.stored_height),
          "-c:v",
          "mpeg4",
          "-q:v",
          std::string(kMpeg4Quantizer),
          "-threads",
          std::string(kEncoderThreads),
          "-fps_mode",
          "passthrough",
          "-enc_time_base",
          encoding.time_base,
          "-f",
          "mov",
          "-y",
          output.string()};
}

std::string piece_concat_list(const std::vector<std::string>& encoded,
                              const std::vector<std::int64_t>& start_times_us) {
  if (encoded.size() != start_times_us.size() || encoded.empty()) {
    throw std::invalid_argument("a concat list needs one start time per piece");
  }
  // Each duration is the difference of two start times rounded once, so the
  // concat demuxer's running sum of them lands every piece at its own
  // rounded start rather than drifting by one rounding per piece.
  std::string list = "ffconcat version 1.0\n";
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i].find_first_of("'\\\n/") != std::string::npos) {
      throw std::invalid_argument("a piece name must be a plain file name: " + encoded[i]);
    }
    list += "file '" + encoded[i] + "'\n";
    if (i + 1 < encoded.size()) {
      list += "duration " + seconds_text(start_times_us[i + 1] - start_times_us[i]) + "\n";
    }
  }
  return list;
}

std::vector<std::string> piece_join_command(const std::filesystem::path& ffmpeg_path,
                                            const JoinInput& input,
                                            const std::filesystem::path& output) {
  std::vector<std::string> command{ffmpeg_path.string(), "-nostdin", "-hide_banner",
                                   "-loglevel", "error"};
  if (input.rotation_degrees != 0) {
    command.insert(command.end(),
                   {"-display_rotation:v:0", std::to_string(input.rotation_degrees)});
  }
  command.insert(command.end(),
                 {"-itsoffset", seconds_text(input.first_frame_offset_us), "-f", "concat", "-i",
                  input.concat_list.string(), "-i", input.source.string(), "-map", "0:v:0", "-map",
                  "1:a?", "-c", "copy", "-map_metadata", "1", "-map_metadata:s:v:0",
                  "1:s:" + std::to_string(input.source_video_index), "-map_chapters", "1", "-f",
                  input.container == ResizeContainer::mp4 ? "mp4" : "mov", "-y",
                  output.string()});
  return command;
}

}  // namespace svp::builder::resize
