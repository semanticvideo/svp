#include "svp/exec/fd_frame_io.hpp"

#include "svp/exec/exec_error.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace svp::exec {
namespace {

// send(2) with MSG_NOSIGNAL where it exists so a vanished peer surfaces as
// EPIPE instead of killing the process; write(2) for pipes.
ssize_t write_some(int fd, const std::byte* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
  const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
  if (sent >= 0 || errno != ENOTSOCK) {
    return sent;
  }
#endif
  return ::write(fd, data, size);
}

[[noreturn]] void throw_io(std::string_view what) {
  throw ExecError(ExecErrorCode::frame_truncated,
                  std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

FdFrameWriter::FdFrameWriter(int fd, FrameLimits limits) : fd_(fd), limits_(limits) {}

void FdFrameWriter::write(const Frame& frame) {
  const std::vector<std::byte> bytes = encode_frame(frame, limits_);
  const std::lock_guard lock(mutex_);
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = write_some(fd_, bytes.data() + written, bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_io("frame write failed");
    }
    written += static_cast<std::size_t>(count);
  }
}

FdFrameReader::FdFrameReader(int fd, FrameLimits limits)
    : fd_(fd), decoder_(limits), chunk_(kFdReadChunkBytes) {}

std::optional<Frame> FdFrameReader::read() {
  while (true) {
    if (auto frame = decoder_.next()) {
      return frame;
    }
    const ssize_t count = ::read(fd_, chunk_.data(), chunk_.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECONNRESET) {
        decoder_.finish();
        return std::nullopt;
      }
      throw_io("frame read failed");
    }
    if (count == 0) {
      decoder_.finish();
      return std::nullopt;
    }
    decoder_.feed(std::span<const std::byte>(chunk_.data(), static_cast<std::size_t>(count)));
  }
}

}  // namespace svp::exec
