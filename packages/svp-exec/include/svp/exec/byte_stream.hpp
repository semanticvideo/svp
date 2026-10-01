#pragma once

#include <cstddef>
#include <span>

namespace svp::exec {

// A reliable, ordered, bidirectional byte stream that the frame protocol runs
// over (plan §4.3: "framing is transport-independent"). Implementations: a
// POSIX socket or pipe pair (FdByteStream) and, on Apple platforms, an
// authenticated TLS connection (svp::exec::remote).
//
// One thread may read while another writes. Concurrent writers must be
// serialised by the caller (StreamFrameWriter does this per frame).
class ByteStream {
 public:
  virtual ~ByteStream() = default;

  // Blocks until at least one byte is available, then copies up to
  // buffer.size() bytes into it and returns the count. Returns 0 at end of
  // stream: the peer closed its side, reset the connection, or this stream
  // was cancelled locally. Throws ExecError(frame_truncated) when the read
  // fails for any other reason.
  [[nodiscard]] virtual std::size_t read_some(std::span<std::byte> buffer) = 0;

  // Writes every byte or throws ExecError(frame_truncated). A peer that has
  // gone away is reported as that error, never as a signal.
  virtual void write_all(std::span<const std::byte> bytes) = 0;
};

}  // namespace svp::exec
