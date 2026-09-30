#include "svp/progress/fd_stream.hpp"

#include <cerrno>
#include <cstddef>
#include <streambuf>
#include <fcntl.h>
#include <unistd.h>

namespace svp::progress {
namespace {

// Duplicates `fd` above the standard descriptors with close-on-exec set, so
// child processes (ffmpeg, ffprobe) never inherit the private progress fd.
int duplicate_cloexec(int fd) {
  if (fd < 0) return -1;
  return ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
}

}  // namespace

class FdStream::Buffer final : public std::streambuf {
 public:
  explicit Buffer(int fd) : fd_(duplicate_cloexec(fd)) {}

  ~Buffer() override {
    if (fd_ >= 0) ::close(fd_);
  }

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  int fd() const noexcept { return fd_; }

 protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
      return traits_type::not_eof(ch);
    }
    const char byte = traits_type::to_char_type(ch);
    return write_all(&byte, 1) == 1 ? ch : traits_type::eof();
  }

  std::streamsize xsputn(const char* data, std::streamsize count) override {
    return write_all(data, count);
  }

 private:
  std::streamsize write_all(const char* data, std::streamsize count) {
    if (fd_ < 0) return 0;
    std::streamsize total = 0;
    while (total < count) {
      const ssize_t written = ::write(
          fd_, data + total, static_cast<std::size_t>(count - total));
      if (written < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (written == 0) break;
      total += written;
    }
    return total;
  }

  int fd_;
};

FdStream::FdStream(int fd)
    : buffer_(std::make_unique<Buffer>(fd)), stream_(buffer_.get()) {
  if (buffer_->fd() < 0) stream_.setstate(std::ios::badbit);
}

FdStream::~FdStream() = default;

int FdStream::fd() const noexcept { return buffer_->fd(); }

std::ostream& FdStream::stream() noexcept { return stream_; }

}  // namespace svp::progress
