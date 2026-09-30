#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/frame_limits.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace svp::exec {

namespace detail {
struct FrameHeader;
}  // namespace detail

// Incremental decoder for the frame layout documented in frame.hpp. Feed
// bytes as they arrive and drain complete frames with next(). The decoder
// rejects an oversize header from its 4-byte length prefix and oversize
// payloads from their header declarations, before buffering either, and
// releases a frame only after every payload's BLAKE3 verifies.
//
// Callers bound their own buffering by draining next() after each feed(): one
// in-progress frame never needs more than
// kFrameLengthPrefixBytes + max_header_bytes + max_total_payload_bytes.
//
// Any ExecError leaves the decoder failed; every later call throws
// ExecError(frame_malformed). A protocol peer that sent a bad frame is not
// resynchronised (plan §4.3: ERROR ends the session).
class FrameDecoder {
 public:
  explicit FrameDecoder(FrameLimits limits = {});
  ~FrameDecoder();
  FrameDecoder(FrameDecoder&&) noexcept;
  FrameDecoder& operator=(FrameDecoder&&) noexcept;
  FrameDecoder(const FrameDecoder&) = delete;
  FrameDecoder& operator=(const FrameDecoder&) = delete;

  void feed(std::span<const std::byte> bytes);

  // Next complete, verified frame, or nullopt when more bytes are needed.
  [[nodiscard]] std::optional<Frame> next();

  // Call at end of stream: throws ExecError(frame_truncated) when a partial
  // frame is still buffered.
  void finish();

  [[nodiscard]] std::size_t buffered_bytes() const noexcept;

 private:
  [[nodiscard]] std::optional<Frame> next_unchecked();
  void require_healthy() const;

  FrameLimits limits_;
  std::vector<std::byte> buffer_;
  // Parsed header of the frame at the front of buffer_, kept while its
  // payloads are still arriving so the header is parsed once per frame.
  std::unique_ptr<detail::FrameHeader> pending_header_;
  std::size_t pending_header_end_ = 0;
  bool failed_ = false;
};

// Decodes a buffer that holds exactly one frame. Throws
// ExecError(frame_truncated) when the frame is incomplete and
// (frame_malformed) when bytes follow it.
[[nodiscard]] Frame decode_frame(std::span<const std::byte> bytes,
                                 const FrameLimits& limits = {});

}  // namespace svp::exec
