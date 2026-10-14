#include "working_copy/working_copy_encoder.hpp"

namespace svp::builder::working_copy {

std::vector<WorkingCopyEncoder> working_copy_encoders() {
  std::vector<WorkingCopyEncoder> encoders;
#if defined(__APPLE__)
  encoders.push_back(WorkingCopyEncoder{
      .name = "hevc_videotoolbox",
      .options = {"-q:v", std::string(kVideoToolboxQuality),
                  // The standard QuickTime sample entry for HEVC.
                  "-tag:v", "hvc1", "-bsf:v",
                  "filter_units=remove_types=" + std::string(kHevcPrefixSeiNalUnitType)},
      .execution_provider = "videotoolbox"});
#endif
  encoders.push_back(WorkingCopyEncoder{.name = "mpeg4",
                                        .options = {"-q:v", std::string(kMpeg4Quantizer)},
                                        .execution_provider = "cpu"});
  return encoders;
}

std::vector<std::string> working_copy_ffmpeg_command(const std::filesystem::path& ffmpeg_path,
                                                     const std::filesystem::path& source,
                                                     int video_stream_index,
                                                     const WorkingCopyGeometry& geometry,
                                                     const WorkingCopyEncoder& encoder,
                                                     const std::filesystem::path& output) {
  std::vector<std::string> command{
      ffmpeg_path.string(), "-nostdin", "-hide_banner", "-loglevel", "error",
      "-noautorotate", "-i", source.string(),
      "-map", "0:" + std::to_string(video_stream_index), "-map", "0:a?", "-c:a", "copy",
      "-vf", "scale=" + std::to_string(geometry.stored_width) + ":" +
                 std::to_string(geometry.stored_height),
      "-c:v", encoder.name};
  command.insert(command.end(), encoder.options.begin(), encoder.options.end());
  command.insert(command.end(), {"-fps_mode", "passthrough", "-enc_time_base", "demux",
                                 "-f", "mov", "-y", output.string()});
  return command;
}

}  // namespace svp::builder::working_copy
