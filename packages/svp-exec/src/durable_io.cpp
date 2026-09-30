#include "durable_io.hpp"

#include "blake3_stream.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace svp::exec::detail {
namespace {

// Files are created owner read/write; the CAS and journal tighten published
// blobs to read-only afterwards.
constexpr mode_t kNewFileMode = S_IRUSR | S_IWUSR;

std::error_code last_error() noexcept {
  return {errno, std::generic_category()};
}

std::error_code write_all(int fd, std::span<const std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    const ssize_t written = ::write(fd, bytes.data(), bytes.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return last_error();
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
  }
  return {};
}

// Reads up to buffer.size() bytes; returns bytes read (0 at end of file).
std::error_code read_some(int fd, std::span<std::byte> buffer, std::size_t& read_bytes) noexcept {
  for (;;) {
    const ssize_t count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return last_error();
    }
    read_bytes = static_cast<std::size_t>(count);
    return {};
  }
}

std::error_code open_fd(const std::filesystem::path& path, int flags, UniqueFd& out) noexcept {
  const int fd = ::open(path.c_str(), flags | O_CLOEXEC, kNewFileMode);
  if (fd < 0) {
    return last_error();
  }
  out = UniqueFd(fd);
  return {};
}

// Streams `input` into BLAKE3 and, when `output` is a valid fd, copies it.
std::error_code stream_fd(int input, int output, FileDigest& digest) {
  std::vector<std::byte> buffer(kFileStreamChunkBytes);
  Blake3Stream hasher;
  std::uint64_t total = 0;
  for (;;) {
    std::size_t count = 0;
    if (const auto error = read_some(input, buffer, count)) {
      return error;
    }
    if (count == 0) {
      break;
    }
    const std::span<const std::byte> chunk(buffer.data(), count);
    hasher.update(chunk);
    total += count;
    if (output >= 0) {
      if (const auto error = write_all(output, chunk)) {
        return error;
      }
    }
  }
  digest = FileDigest{.digest = hasher.finalize(), .bytes = total};
  return {};
}

}  // namespace

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

UniqueFd::~UniqueFd() {
  reset();
}

void UniqueFd::reset() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

std::error_code sync_fd(int fd) noexcept {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return {};
  }
  // Filesystems without F_FULLFSYNC support (some network and FUSE mounts)
  // fall through to plain fsync.
#endif
  if (::fsync(fd) != 0) {
    return last_error();
  }
  return {};
}

std::error_code sync_directory(const std::filesystem::path& directory) noexcept {
  UniqueFd fd;
  if (const auto error = open_fd(directory, O_RDONLY, fd)) {
    return error;
  }
  if (::fsync(fd.get()) != 0) {
    // Some filesystems reject fsync on directories; the rename is still
    // atomic, only its durability across power loss is weaker.
    if (errno == EINVAL || errno == ENOTSUP) {
      return {};
    }
    return last_error();
  }
  return {};
}

std::error_code hash_file(const std::filesystem::path& path, FileDigest& out) {
  UniqueFd fd;
  if (const auto error = open_fd(path, O_RDONLY, fd)) {
    return error;
  }
  return stream_fd(fd.get(), -1, out);
}

std::error_code write_new_file_synced(const std::filesystem::path& path,
                                      std::span<const std::byte> bytes) {
  UniqueFd fd;
  if (const auto error = open_fd(path, O_WRONLY | O_CREAT | O_EXCL, fd)) {
    return error;
  }
  if (const auto error = write_all(fd.get(), bytes)) {
    return error;
  }
  return sync_fd(fd.get());
}

std::error_code copy_to_new_file_synced(const std::filesystem::path& source,
                                        const std::filesystem::path& destination,
                                        FileDigest& source_digest) {
  UniqueFd input;
  if (const auto error = open_fd(source, O_RDONLY, input)) {
    return error;
  }
  UniqueFd output;
  if (const auto error = open_fd(destination, O_WRONLY | O_CREAT | O_EXCL, output)) {
    return error;
  }
  if (const auto error = stream_fd(input.get(), output.get(), source_digest)) {
    return error;
  }
  return sync_fd(output.get());
}

}  // namespace svp::exec::detail
