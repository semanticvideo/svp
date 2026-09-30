#include "svp/exec/frame_decoder.hpp"

#include "frame_header.hpp"
#include "svp/exec/exec_error.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace svp::exec {
namespace {

std::uint32_t read_big_endian_u32(std::span<const std::byte> bytes) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < kFrameLengthPrefixBytes; ++index) {
    value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[index]);
  }
  return value;
}

}  // namespace

FrameDecoder::FrameDecoder(FrameLimits limits) : limits_(limits) {}
FrameDecoder::~FrameDecoder() = default;
FrameDecoder::FrameDecoder(FrameDecoder&&) noexcept = default;
FrameDecoder& FrameDecoder::operator=(FrameDecoder&&) noexcept = default;

void FrameDecoder::require_healthy() const {
  if (failed_) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    "frame decoder already rejected this stream");
  }
}

void FrameDecoder::feed(std::span<const std::byte> bytes) {
  require_healthy();
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameDecoder::next() {
  require_healthy();
  try {
    return next_unchecked();
  } catch (...) {
    failed_ = true;
    throw;
  }
}

std::optional<Frame> FrameDecoder::next_unchecked() {
  if (!pending_header_) {
    if (buffer_.size() < kFrameLengthPrefixBytes) {
      return std::nullopt;
    }
    const std::uint32_t header_bytes = read_big_endian_u32(buffer_);
    if (header_bytes == 0) {
      throw ExecError(ExecErrorCode::frame_malformed,
                      "frame header length must be at least 1 byte");
    }
    if (header_bytes > limits_.max_header_bytes) {
      throw ExecError(ExecErrorCode::frame_header_too_large,
                      "frame header of " + std::to_string(header_bytes) +
                          " bytes exceeds the " +
                          std::to_string(limits_.max_header_bytes) +
                          "-byte header limit");
    }
    const std::size_t header_end = kFrameLengthPrefixBytes + header_bytes;
    if (buffer_.size() < header_end) {
      return std::nullopt;
    }
    const std::string_view header_text(
        reinterpret_cast<const char*>(buffer_.data()) + kFrameLengthPrefixBytes,
        header_bytes);
    pending_header_ = std::make_unique<detail::FrameHeader>(
        detail::decode_frame_header(header_text, limits_));
    pending_header_end_ = header_end;
  }

  const std::size_t frame_end =
      pending_header_end_ +
      static_cast<std::size_t>(
          detail::declared_payload_bytes(pending_header_->payloads));
  if (buffer_.size() < frame_end) {
    return std::nullopt;
  }

  Frame frame{.type = pending_header_->type,
              .body = std::move(pending_header_->body),
              .payloads = {}};
  std::size_t offset = pending_header_end_;
  for (std::size_t index = 0; index < pending_header_->payloads.size(); ++index) {
    const detail::PayloadDeclaration& declaration =
        pending_header_->payloads[index];
    const auto length = static_cast<std::size_t>(declaration.bytes);
    const std::span<const std::byte> payload(buffer_.data() + offset, length);
    if (blake3_digest(payload) != declaration.blake3) {
      throw ExecError(ExecErrorCode::payload_hash_mismatch,
                      "frame payload " + std::to_string(index) +
                          " does not match its declared BLAKE3");
    }
    frame.payloads.emplace_back(payload.begin(), payload.end());
    offset += length;
  }

  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(frame_end));
  pending_header_.reset();
  pending_header_end_ = 0;
  return frame;
}

void FrameDecoder::finish() {
  require_healthy();
  if (!buffer_.empty()) {
    failed_ = true;
    throw ExecError(ExecErrorCode::frame_truncated,
                    "stream ended inside a frame (" +
                        std::to_string(buffer_.size()) + " bytes buffered)");
  }
}

std::size_t FrameDecoder::buffered_bytes() const noexcept {
  return buffer_.size();
}

Frame decode_frame(std::span<const std::byte> bytes, const FrameLimits& limits) {
  FrameDecoder decoder(limits);
  decoder.feed(bytes);
  std::optional<Frame> frame = decoder.next();
  if (!frame) {
    throw ExecError(ExecErrorCode::frame_truncated,
                    "buffer ends before the frame is complete");
  }
  if (decoder.buffered_bytes() != 0) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    "bytes follow the frame in a single-frame buffer");
  }
  return std::move(*frame);
}

}  // namespace svp::exec
