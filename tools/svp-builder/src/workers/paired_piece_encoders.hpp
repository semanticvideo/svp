#pragma once

// The coordinator side of resize.piece (resize/piece_task.hpp): this Mac's
// paired workers encoding pieces of a split resize (resize/split_resize.hpp)
// alongside it. For each paired Mac, one session:
//   1. reaches it by pairing id and sends HELLO declaring one resize.piece
//      slot and this runtime, then the runtime when that Mac lacks it
//      (waiting first when an earlier session left it switching runtimes);
//   2. takes the next piece, sends it (BLOB_PUT, verified there by BLAKE3),
//      assigns it, waits for its result (renewed by each heartbeat), fetches
//      the encoded piece (BLOB_GET, verified by BLAKE3), and releases both
//      there (BLOB_RELEASE, worker protocol 1.2); then the next piece;
//   3. gives a piece back for another Mac, and stops, when that Mac cannot
//      be reached, turns the session or the piece away, goes silent for a
//      lease period, or fails it.
// No Mac is required: a split resize finishes on this Mac alone.

#include "resize/split_resize.hpp"

#include <filesystem>

namespace svp::builder::workers {

class PairedPieceEncoders final : public svp::builder::resize::RemotePieceEncoders {
 public:
  // `ffmpeg_path`: this Mac's ffmpeg, whose build every worker must have.
  explicit PairedPieceEncoders(std::filesystem::path ffmpeg_path);

  void run(svp::builder::resize::PieceQueue& queue,
           const svp::builder::resize::PieceEncoding& encoding,
           const std::function<void(const std::string&)>& report,
           const svp::exec::CancellationToken* cancellation) override;

 private:
  std::filesystem::path ffmpeg_path_;
};

}  // namespace svp::builder::workers
