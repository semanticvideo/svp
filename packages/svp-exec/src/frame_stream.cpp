#include "svp/exec/frame_stream.hpp"

#include <span>

namespace svp::exec {

StreamFrameReader::StreamFrameReader(ByteStream& stream, FrameLimits limits)
    : stream_(stream), decoder_(limits), chunk_(kStreamReadChunkBytes) {}

std::optional<Frame> StreamFrameReader::read() {
  while (true) {
    if (auto frame = decoder_.next()) {
      return frame;
    }
    const std::size_t count = stream_.read_some(chunk_);
    if (count == 0) {
      decoder_.finish();
      return std::nullopt;
    }
    decoder_.feed(std::span<const std::byte>(chunk_.data(), count));
  }
}

StreamFrameWriter::StreamFrameWriter(ByteStream& stream, FrameLimits limits)
    : stream_(stream), limits_(limits) {}

void StreamFrameWriter::write(const Frame& frame) {
  const std::vector<std::byte> bytes = encode_frame(frame, limits_);
  const std::lock_guard lock(mutex_);
  stream_.write_all(bytes);
}

}  // namespace svp::exec
