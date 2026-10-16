#pragma once

// The packets of one stream of a media file, in file (decode) order, as
// ffprobe reads them: what a split resize (split_resize.hpp) cuts the source
// by, and checks each piece and the joined media against.

#include "svp/exec/cancellation_token.hpp"
#include "svp/media/rational.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::resize {

struct SourcePacket {
  // Presentation time in the stream's time base; nullopt when the file
  // stores none (some AVI files).
  std::optional<std::int64_t> pts;
  std::int64_t size = 0;
  bool keyframe = false;
  // Decoded but never shown (an edit list hides it), or marked corrupt.
  bool hidden = false;
};

struct PacketTable {
  svp::media::Rational time_base{0, 1};
  std::vector<SourcePacket> packets;
};

// ffprobe's `-show_entries packet=pts,size,flags:stream=time_base -of csv`
// report of one stream. nullopt when it is not one.
[[nodiscard]] std::optional<PacketTable> parse_packet_report(std::string_view csv);

// The packet table of stream `stream` (an ffprobe stream specifier) of
// `file`. nullopt, with `error` saying why, when ffprobe cannot read it or
// the build was cancelled. `scratch_dir` holds ffprobe's output, named after
// `file`, so files of different names can be read at once.
[[nodiscard]] std::optional<PacketTable> probe_packets(
    const std::filesystem::path& ffprobe_path, const std::filesystem::path& file,
    const std::string& stream, const std::filesystem::path& scratch_dir,
    const svp::exec::CancellationToken* cancellation, std::string& error);

}  // namespace svp::builder::resize
