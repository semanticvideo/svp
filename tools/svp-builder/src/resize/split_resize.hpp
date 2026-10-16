#pragma once

// --max-resolution in pieces (resized_source.hpp): the source's primary
// video is cut, without re-encoding, at clean keyframes about kPieceSeconds
// apart (piece_plan.hpp); this Mac and, when the build has them, paired
// worker Macs (RemotePieceEncoders) each take the next piece as they come
// free and encode it with the one command every Mac runs
// (piece_encode_command); and the encoded pieces are joined, end to end at
// their source times, with every audio stream of the source.
//
// The cut, the encodes, and the join keep each frame's source time, and the
// result is checked before it is used: every piece holds exactly its
// packets of the source (presentation times and sizes), and the joined
// video has one frame for each source frame, at the source frame's time
// (within the encoder time base's rounding, kStartOffsetToleranceUs), with
// the source's display matrix. A source that cannot be cut so, or a result
// that fails a check, is resized in one pass instead (resize_ffmpeg_command).
//
// The pieces depend on the source alone, so the resized media is the same
// bytes whichever Macs, and how many, encoded them.

#include "resize/piece_commands.hpp"
#include "resize/resize_geometry.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/media/media_probe.hpp"

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace svp::builder::resize {

struct PieceJob {
  std::size_t index = 0;
  // The cut piece, and where its encoding is written.
  std::filesystem::path piece;
  std::filesystem::path encoded;
};

// The pieces of one split resize still to encode. This Mac and every worker
// Mac take the next one as they come free, so a faster Mac encodes more.
class PieceQueue {
 public:
  explicit PieceQueue(std::vector<PieceJob> jobs);

  // The next piece to encode, in order. Waits while every piece left is
  // being encoded elsewhere (one may be given back). nullopt once every
  // piece is encoded or the queue is closed.
  [[nodiscard]] std::optional<PieceJob> take();
  // `index` is encoded: its cut piece is removed, as nothing reads it again.
  void encoded(std::size_t index);
  // `index` could not be encoded where it was taken: another Mac takes it.
  void give_back(std::size_t index);
  // No more pieces are handed out, and take() returns nullopt.
  void close();

  [[nodiscard]] bool all_encoded() const;
  [[nodiscard]] bool closed() const;
  // Until every piece is encoded or the queue is closed.
  void wait() const;

 private:
  std::vector<PieceJob> jobs_;
  std::set<std::size_t> waiting_;
  std::set<std::size_t> done_;
  bool closed_ = false;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
};

// Encodes pieces on other Macs: the build's paired workers.
class RemotePieceEncoders {
 public:
  virtual ~RemotePieceEncoders() = default;

  // Takes pieces from `queue` for every Mac it reaches and encodes each
  // there with piece_encode_command and `encoding`, writing it to the
  // piece's `encoded` path, until `queue` hands out no more; a piece a Mac
  // cannot encode is given back. Returns once every Mac it used has
  // stopped. `report` takes one line per event worth telling. Never throws.
  virtual void run(PieceQueue& queue, const PieceEncoding& encoding,
                   const std::function<void(const std::string&)>& report,
                   const svp::exec::CancellationToken* cancellation) = 0;
};

struct SplitResizeRequest {
  std::filesystem::path source_path;
  svp::media::VideoStreamProbe video;
  ResizeGeometry geometry;
  ResizeContainer container = ResizeContainer::quicktime;
  // The file written; the pieces are cut beside it, in a directory removed
  // when the resize ends.
  std::filesystem::path output_path;
  std::filesystem::path ffmpeg_path;
  std::filesystem::path ffprobe_path;
  // Holds ffprobe's output and ffmpeg's logs.
  std::filesystem::path scratch_dir;
  // nullptr: this Mac encodes every piece.
  std::shared_ptr<RemotePieceEncoders> remote;
  std::function<void(const std::string&)> report;
  const svp::exec::CancellationToken* cancellation = nullptr;
};

struct SplitResizeOutcome {
  enum class Status {
    // `output_path` holds the joined, checked resized media.
    written,
    // The source is not cut (why): it is resized in one pass.
    not_split,
    // The split resize did not finish or its result failed a check (why):
    // it is resized in one pass.
    failed,
    cancelled,
  };
  Status status = Status::not_split;
  std::string why;
  std::size_t pieces = 0;
  // How many of the pieces other Macs encoded.
  std::size_t remote_pieces = 0;
};

[[nodiscard]] SplitResizeOutcome split_resize(const SplitResizeRequest& request);

}  // namespace svp::builder::resize
