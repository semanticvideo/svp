#include "svp/exec/fd_frame_io.hpp"

#include "svp/exec/exec_error.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

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

FdByteStream::FdByteStream(int read_fd, int write_fd)
    : read_fd_(read_fd), write_fd_(write_fd) {}

std::size_t FdByteStream::read_some(std::span<std::byte> buffer) {
  while (true) {
    const ssize_t count = ::read(read_fd_, buffer.data(), buffer.size());
    if (count >= 0) {
      return static_cast<std::size_t>(count);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == ECONNRESET) {
      return 0;
    }
    throw_io("frame read failed");
  }
}

void FdByteStream::write_all(std::span<const std::byte> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count =
        write_some(write_fd_, bytes.data() + written, bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_io("frame write failed");
    }
    written += static_cast<std::size_t>(count);
  }
}

FdFrameWriter::FdFrameWriter(int fd, FrameLimits limits)
    : stream_(-1, fd), writer_(stream_, limits) {}

void FdFrameWriter::write(const Frame& frame) { writer_.write(frame); }

FdFrameReader::FdFrameReader(int fd, FrameLimits limits)
    : stream_(fd, -1), reader_(stream_, limits) {}

std::optional<Frame> FdFrameReader::read() { return reader_.read(); }

}  // namespace svp::exec
