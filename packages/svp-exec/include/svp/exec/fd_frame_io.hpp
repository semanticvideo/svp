#pragma once

#include "svp/exec/byte_stream.hpp"
#include "svp/exec/frame.hpp"
#include "svp/exec/frame_limits.hpp"
#include "svp/exec/frame_stream.hpp"

#include <cstddef>
#include <optional>

namespace svp::exec {

// A ByteStream over POSIX descriptors: one pipe or stream socket for each
// direction, or the same socket for both. Writing to a closed socket fails
// with ExecError(frame_truncated) instead of raising SIGPIPE where the
// platform allows (MSG_NOSIGNAL); callers that write to pipes, or on
// platforms without it, set SO_NOSIGPIPE or ignore SIGPIPE themselves. A
// connection reset reads as end of stream. Does not own the descriptors.
class FdByteStream final : public ByteStream {
 public:
  FdByteStream(int read_fd, int write_fd);
  [[nodiscard]] std::size_t read_some(std::span<std::byte> buffer) override;
  void write_all(std::span<const std::byte> bytes) override;

 private:
  int read_fd_;
  int write_fd_;
};

// Writes whole frames to a POSIX pipe or stream socket. Thread-safe: frames
// from concurrent writers never interleave. Does not own the descriptor.
class FdFrameWriter final : public FrameWriter {
 public:
  explicit FdFrameWriter(int fd, FrameLimits limits = {});
  void write(const Frame& frame) override;

 private:
  FdByteStream stream_;
  StreamFrameWriter writer_;
};

// Reads frames from a POSIX pipe or stream socket. Single reader. Does not
// own the descriptor. See FrameReader::read for end-of-stream and errors.
class FdFrameReader final : public FrameReader {
 public:
  explicit FdFrameReader(int fd, FrameLimits limits = {});
  [[nodiscard]] std::optional<Frame> read() override;

 private:
  FdByteStream stream_;
  StreamFrameReader reader_;
};

}  // namespace svp::exec
