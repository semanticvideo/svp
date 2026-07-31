#include "svp/package/embedded_package_session.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace svp::package {

namespace {

bool read_file_identity(
    const std::filesystem::path& path,
    std::uint64_t& device,
    std::uint64_t& inode,
    std::uint64_t& file_size,
    std::int64_t& modified_seconds,
    std::int64_t& modified_nanoseconds,
    std::int64_t& changed_seconds,
    std::int64_t& changed_nanoseconds) {
#if defined(_WIN32)
  const HANDLE file = CreateFileW(
      path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  BY_HANDLE_FILE_INFORMATION status {};
  const bool read = GetFileInformationByHandle(file, &status) != 0;
  CloseHandle(file);
  if (!read) {
    return false;
  }

  const auto split_file_time = [](const FILETIME& value,
                                  std::int64_t& seconds,
                                  std::int64_t& nanoseconds) {
    ULARGE_INTEGER ticks {};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    seconds = static_cast<std::int64_t>(ticks.QuadPart / 10000000ULL);
    nanoseconds = static_cast<std::int64_t>(
        (ticks.QuadPart % 10000000ULL) * 100ULL);
  };

  device = status.dwVolumeSerialNumber;
  inode = (static_cast<std::uint64_t>(status.nFileIndexHigh) << 32U) |
          status.nFileIndexLow;
  file_size = (static_cast<std::uint64_t>(status.nFileSizeHigh) << 32U) |
              status.nFileSizeLow;
  split_file_time(status.ftLastWriteTime, modified_seconds,
                  modified_nanoseconds);
  split_file_time(status.ftCreationTime, changed_seconds,
                  changed_nanoseconds);
#else
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0 || status.st_size < 0) {
    return false;
  }
  device = static_cast<std::uint64_t>(status.st_dev);
  inode = static_cast<std::uint64_t>(status.st_ino);
  file_size = static_cast<std::uint64_t>(status.st_size);
#if defined(__APPLE__)
  modified_seconds = status.st_mtimespec.tv_sec;
  modified_nanoseconds = status.st_mtimespec.tv_nsec;
  changed_seconds = status.st_ctimespec.tv_sec;
  changed_nanoseconds = status.st_ctimespec.tv_nsec;
#else
  modified_seconds = status.st_mtim.tv_sec;
  modified_nanoseconds = status.st_mtim.tv_nsec;
  changed_seconds = status.st_ctim.tv_sec;
  changed_nanoseconds = status.st_ctim.tv_nsec;
#endif
#endif
  return true;
}

}  // namespace

thread_local VerifiedEmbeddedPackageSession*
    VerifiedEmbeddedPackageSession::current_ = nullptr;

VerifiedEmbeddedPackageSession::VerifiedEmbeddedPackageSession(
    const std::filesystem::path& path)
    : path_(path) {
  const bool identity_captured = read_file_identity(
      path_, device_, inode_, file_size_, modified_seconds_,
      modified_nanoseconds_, changed_seconds_, changed_nanoseconds_);
  inspection_ = inspect_embedded_svpi(path, true);
  active_ = identity_captured &&
            inspection_.has_single_valid_embedding() &&
            inspection_.embeddings.front().hash_verified &&
            inspection_.embeddings.front().hash_matches &&
            matches(path_);
  if (active_) {
    previous_ = current_;
    current_ = this;
  }
}

VerifiedEmbeddedPackageSession::~VerifiedEmbeddedPackageSession() {
  if (!active_) {
    return;
  }
  if (current_ == this) {
    current_ = previous_;
    return;
  }
  for (auto* session = current_; session != nullptr;
       session = session->previous_) {
    if (session->previous_ == this) {
      session->previous_ = previous_;
      return;
    }
  }
}

bool VerifiedEmbeddedPackageSession::valid() const noexcept {
  return active_;
}

const EmbeddedSvpiInspection&
VerifiedEmbeddedPackageSession::inspection() const noexcept {
  return inspection_;
}

bool VerifiedEmbeddedPackageSession::matches(
    const std::filesystem::path& path) const {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t file_size = 0;
  std::int64_t modified_seconds = 0;
  std::int64_t modified_nanoseconds = 0;
  std::int64_t changed_seconds = 0;
  std::int64_t changed_nanoseconds = 0;
  return read_file_identity(
             path, device, inode, file_size, modified_seconds,
             modified_nanoseconds, changed_seconds, changed_nanoseconds) &&
         device == device_ && inode == inode_ && file_size == file_size_ &&
         modified_seconds == modified_seconds_ &&
         modified_nanoseconds == modified_nanoseconds_ &&
         changed_seconds == changed_seconds_ &&
         changed_nanoseconds == changed_nanoseconds_;
}

std::uint64_t VerifiedEmbeddedPackageSession::payload_offset() const noexcept {
  return active_ ? inspection_.embeddings.front().payload_offset : 0;
}

std::uint64_t VerifiedEmbeddedPackageSession::payload_size() const noexcept {
  return active_ ? inspection_.embeddings.front().payload_size : 0;
}

bool VerifiedEmbeddedPackageSession::active_range_for(
    const std::filesystem::path& path,
    std::uint64_t& payload_offset,
    std::uint64_t& payload_size) {
  if (current_ == nullptr || !current_->matches(path)) {
    return false;
  }
  payload_offset = current_->payload_offset();
  payload_size = current_->payload_size();
  return true;
}

}  // namespace svp::package
