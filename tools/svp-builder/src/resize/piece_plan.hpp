#pragma once

// Where a split resize (split_resize.hpp) cuts the source's primary video:
// into pieces that start at keyframes every frame after which decodes
// without any packet before it, about kPieceSeconds apart.
//
// A keyframe K is a clean cut when every packet before K in the file is
// shown before K and every packet after K is shown after it. The packets
// before K and the frames shown before K are then the same set, so a piece
// that runs from one clean cut to the next holds exactly the frames shown
// between them, and the pieces together hold every frame once.
//
// The plan depends on the source alone, never on how many Macs encode its
// pieces: the resized media is the same whichever Macs ran.

#include "resize/source_packets.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::resize {

// The time from one cut to the next the plan aims for: cut n is the first
// clean keyframe n times this long or more after the first frame. Each piece is one Mac's task, so shorter pieces
// spread a video over more Macs and leave less of it to the slowest Mac at
// the end; each piece also costs one ffmpeg start, one transfer, and an
// encoder keyframe at its cut. Measured on a 28-minute 3840x2160 H.264
// video (61 Mb/s) on an M4 Mac: a 30 s piece encodes in about 5 s, of which
// about 0.4 s is ffmpeg starting and finishing, and takes about 2 s to send
// over gigabit Ethernet; the video's 57 pieces keep several Macs busy to
// the end.
inline constexpr std::int64_t kPieceSeconds = 30;

struct PiecePlan {
  // The index, in file order, of each piece's first packet, ascending; the
  // first is 0. Piece i runs to the next piece's first packet, the last to
  // the end of the stream.
  std::vector<std::size_t> first_packets;
};

struct PiecePlanResult {
  std::optional<PiecePlan> plan;
  // Why the source is not split, when it is not.
  std::string why_not;
};

// The pieces of `table`, or why it is resized whole: a packet has no
// presentation time or is hidden, the stream does not start on a clean cut,
// or it has no clean cut kPieceSeconds or more after its start (one piece).
[[nodiscard]] PiecePlanResult plan_pieces(const PacketTable& table);

}  // namespace svp::builder::resize
