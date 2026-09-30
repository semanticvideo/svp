#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/frame_decoder.hpp"
#include "svp/exec/frame_limits.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace svp::exec {

// Bytes requested per read(2). 64 KiB matches the default pipe and local
// socket buffer on macOS, so one read usually drains what the peer wrote.
inline constexpr std::size_t kFdReadChunkBytes = 64U * 1024U;

// Writes whole frames to a POSIX pipe or stream socket. Thread-safe: frames
// from concurrent writers never interleave. Writing to a closed socket fails
// with ExecError(frame_truncated) instead of raising SIGPIPE where the
// platform allows (MSG_NOSIGNAL); callers that write to pipes, or on
// platforms without it, set SO_NOSIGPIPE or ignore SIGPIPE themselves. Does
// not own the descriptor.
class FdFrameWriter {
 public:
  explicit FdFrameWriter(int fd, FrameLimits limits = {});
  void write(const Frame& frame);

 private:
  int fd_;
  FrameLimits limits_;
  std::mutex mutex_;
};

// Reads frames from a POSIX pipe or stream socket. Single reader. Does not
// own the descriptor.
class FdFrameReader {
 public:
  explicit FdFrameReader(int fd, FrameLimits limits = {});

  // Blocks until a complete, verified frame arrives. Returns nullopt at end
  // of stream on a frame boundary (the peer closed cleanly or died between
  // frames). Throws the FrameDecoder errors for bad bytes,
  // ExecError(frame_truncated) when the stream ends inside a frame or the
  // read fails.
  [[nodiscard]] std::optional<Frame> read();

 private:
  int fd_;
  FrameDecoder decoder_;
  std::vector<std::byte> chunk_;
};

}  // namespace svp::exec
