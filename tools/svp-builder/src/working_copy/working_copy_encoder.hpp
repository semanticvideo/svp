#pragma once

// How the working copy is encoded (working_copy.hpp): which encoders are
// tried, in which order, and the ffmpeg command for one of them.
//
// The command keeps every frame at its source timestamp: no frame is added
// or dropped (-fps_mode passthrough), the encoder runs in the source stream's
// time base (-enc_time_base demux) so variable frame rates and fine time
// bases survive, and every audio stream is copied without re-encoding, so the
// copy starts where the source does and a decoder's -ss seek lands on the
// same frame in both files. The source's rotation metadata is kept rather
// than applied (-noautorotate), so decoders orient the copy's frames exactly
// as they orient the source's. Frames are scaled to the planned stored size.

#include "working_copy/working_copy_geometry.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::working_copy {

// VideoToolbox HEVC quality (-q:v, 1..100). Measured on a 60 s 3840x2160
// H.264 slice of the 17-minute RV Park video on an M4 Mac: q 80 made a 48 MB
// 1080p copy of the 375 MB slice in 10 s; a distributed build took 66 s
// instead of 137 s (evidence crops 16 s instead of 56 s) and read the same
// OCR text, with no numbers lost.
inline constexpr std::string_view kVideoToolboxQuality = "80";

// MPEG-4 Part 2 quantizer (-q:v, 1 best..31), the fallback every ffmpeg build
// has, including the LGPL runtime bundle, which has no VideoToolbox. Measured
// on the same slice and Mac with the bundled ffmpeg: q 2 made an 82 MB copy in
// 11 s with every frame timestamp identical to the source's; a distributed
// build took 81 s instead of 142 s and lost none of the source build's
// numbers. On the 40 s comments slice 3 of 25 numbers changed, all
// low-confidence single readings, as 2 did with VideoToolbox q 80.
inline constexpr std::string_view kMpeg4Quantizer = "2";

// HEVC NAL unit type of a prefix SEI message (ITU-T H.265 table 7-1).
// VideoToolbox writes one per run whose payload changes from run to run, so
// it is removed: the copy is then the same bytes every time it is made from
// the same source on the same Mac, which is what lets --resume reuse tasks
// keyed by the copy's BLAKE3. Decoding does not use it.
inline constexpr std::string_view kHevcPrefixSeiNalUnitType = "39";

// Container of the working copy: QuickTime keeps per-track time bases and
// edit lists exactly and takes the audio codecs of common camera and phone
// files (AAC, PCM, ALAC) by stream copy.
inline constexpr std::string_view kWorkingCopyFileName = "working-copy.mov";

struct WorkingCopyEncoder {
  // ffmpeg encoder name.
  std::string name;
  // Its options, after -c:v <name>.
  std::vector<std::string> options;
  // Where it runs, as processor records name it: "videotoolbox" or "cpu".
  std::string execution_provider;
};

// The encoders to try, preferred first: VideoToolbox HEVC on Apple
// platforms, then MPEG-4 Part 2 in software. An encoder this ffmpeg lacks, or
// whose hardware session cannot start, fails at once and the next is tried.
[[nodiscard]] std::vector<WorkingCopyEncoder> working_copy_encoders();

// The ffmpeg command that writes the copy of `source`'s stream
// `video_stream_index` (its index in the file, as ffprobe numbers it) to
// `output`.
[[nodiscard]] std::vector<std::string> working_copy_ffmpeg_command(
    const std::filesystem::path& ffmpeg_path, const std::filesystem::path& source,
    int video_stream_index, const WorkingCopyGeometry& geometry,
    const WorkingCopyEncoder& encoder, const std::filesystem::path& output);

}  // namespace svp::builder::working_copy
