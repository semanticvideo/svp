#pragma once

#include "svp/exec/byte_stream.hpp"
#include "svp/exec/frame.hpp"
#include "svp/exec/frame_decoder.hpp"
#include "svp/exec/frame_limits.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace svp::exec {

// Bytes requested per ByteStream::read_some. 64 KiB matches the default pipe
// and local socket buffer on macOS, so one read usually drains what the peer
// wrote; network streams buffer internally and serve reads of this size from
// memory.
inline constexpr std::size_t kStreamReadChunkBytes = 64U * 1024U;

// Source of whole, verified frames (plan §4.3). Single reader.
class FrameReader {
 public:
  virtual ~FrameReader() = default;

  // Blocks until a complete, verified frame arrives. Returns nullopt at end
  // of stream on a frame boundary (the peer closed cleanly or died between
  // frames). Throws the FrameDecoder errors for bad bytes and
  // ExecError(frame_truncated) when the stream ends inside a frame or the
  // read fails.
  [[nodiscard]] virtual std::optional<Frame> read() = 0;
};

// Sink for whole frames. Implementations are thread-safe: frames from
// concurrent writers never interleave. Throws ExecError(frame_truncated) when
// the peer is gone.
class FrameWriter {
 public:
  virtual ~FrameWriter() = default;
  virtual void write(const Frame& frame) = 0;
};

// Frames decoded from any ByteStream. Does not own the stream.
class StreamFrameReader final : public FrameReader {
 public:
  explicit StreamFrameReader(ByteStream& stream, FrameLimits limits = {});
  [[nodiscard]] std::optional<Frame> read() override;

 private:
  ByteStream& stream_;
  FrameDecoder decoder_;
  std::vector<std::byte> chunk_;
};

// Frames encoded onto any ByteStream, one whole frame per lock. Does not own
// the stream.
class StreamFrameWriter final : public FrameWriter {
 public:
  explicit StreamFrameWriter(ByteStream& stream, FrameLimits limits = {});
  void write(const Frame& frame) override;

 private:
  ByteStream& stream_;
  FrameLimits limits_;
  std::mutex mutex_;
};

}  // namespace svp::exec
