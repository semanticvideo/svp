#pragma once

// The ffmpeg commands of a split resize (split_resize.hpp): cutting the
// source's primary video into pieces, encoding one piece (the same command
// on this Mac and on a worker, so a piece is the same bytes wherever it is
// encoded), and joining the encoded pieces with the source's audio into the
// resized media.
//
// Every command keeps each frame's source time (-copyts on the cut and the
// encodes), so a piece's frames are checked against the source's packets
// time by time, and the join lays the pieces end to end at their source
// times.

#include "resize/resize_encoder.hpp"
#include "resize/resize_geometry.hpp"

#include "svp/media/rational.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::resize {

// The containers a piece may be cut into, tried in order until one holds
// every packet of the stream with its presentation time and size unchanged
// (split_resize.hpp checks): NUT stores each packet's time in the stream's
// own time base and takes most codecs; QuickTime takes codecs NUT has no
// tag for (DNxHD); Matroska takes the rest.
struct PieceContainer {
  std::string_view muxer;
  std::string_view extension;
};
inline constexpr std::array<PieceContainer, 3> kPieceContainers{
    PieceContainer{"nut", "nut"}, PieceContainer{"mov", "mov"},
    PieceContainer{"matroska", "mkv"}};

// What every Mac encodes a piece to: the resized frame size and the encoder
// time base, the source stream's when MPEG-4 Part 2 can store it, otherwise
// kFineTimeBase, as resize_ffmpeg_command's encoder uses.
struct PieceEncoding {
  int stored_width = 0;
  int stored_height = 0;
  std::string time_base;
};

[[nodiscard]] PieceEncoding piece_encoding(const ResizeGeometry& geometry,
                                           const svp::media::Rational& source_time_base);

// Cuts stream `video_index` of `source` into `pattern` files (an ffmpeg
// %05d pattern), a new one starting at each packet index of `cuts` after the
// first (PiecePlan::first_packets), without re-encoding.
[[nodiscard]] std::vector<std::string> piece_cut_command(
    const std::filesystem::path& ffmpeg_path, const std::filesystem::path& source,
    std::int32_t video_index, const std::vector<std::size_t>& cuts,
    const PieceContainer& container, const std::filesystem::path& pattern);

// Encodes `piece` to `output` (QuickTime), each frame at its source time.
[[nodiscard]] std::vector<std::string> piece_encode_command(
    const std::filesystem::path& ffmpeg_path, const std::filesystem::path& piece,
    const PieceEncoding& encoding, const std::filesystem::path& output);

// The ffconcat list that lays `encoded` (file names in the list's directory)
// end to end, each lasting from its first frame's source time
// (`start_times_us`, microseconds after the first piece's) to the next's.
[[nodiscard]] std::string piece_concat_list(const std::vector<std::string>& encoded,
                                            const std::vector<std::int64_t>& start_times_us);

struct JoinInput {
  std::filesystem::path concat_list;
  std::filesystem::path source;
  std::int32_t source_video_index = 0;
  // How long after the source file's start the first frame is shown.
  std::int64_t first_frame_offset_us = 0;
  // The source's rotation metadata, which the pieces do not carry.
  std::int32_t rotation_degrees = 0;
  ResizeContainer container = ResizeContainer::quicktime;
};

// Writes the joined video, every audio stream of the source (stream copy,
// as resize_ffmpeg_command does), and the source's metadata and chapters to
// `output`.
[[nodiscard]] std::vector<std::string> piece_join_command(const std::filesystem::path& ffmpeg_path,
                                                          const JoinInput& input,
                                                          const std::filesystem::path& output);

}  // namespace svp::builder::resize
