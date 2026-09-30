#include "file_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace svp::exec::detail {
namespace {

// Each retry means another process deleted the lock file between our open and
// our flock, which only happens while a holder is tearing down. A handful of
// retries covers back-to-back teardowns; more means something is deleting
// lock files in a loop, which is reported as a failure instead of spinning.
constexpr int kMaxRelinkRetries = 8;

constexpr mode_t kLockFileMode = S_IRUSR | S_IWUSR;

int flock_operation(LockMode mode, LockWait wait) noexcept {
  int operation = mode == LockMode::shared ? LOCK_SH : LOCK_EX;
  if (wait == LockWait::try_once) {
    operation |= LOCK_NB;
  }
  return operation;
}

bool same_file(int fd, const std::filesystem::path& path) noexcept {
  struct stat by_fd{};
  struct stat by_path{};
  if (::fstat(fd, &by_fd) != 0 || ::stat(path.c_str(), &by_path) != 0) {
    return false;
  }
  return by_fd.st_dev == by_path.st_dev && by_fd.st_ino == by_path.st_ino;
}

}  // namespace

std::error_code FileLock::write_note(std::string_view note) const {
  if (::ftruncate(fd_.get(), 0) != 0) {
    return {errno, std::generic_category()};
  }
  const ssize_t written = ::pwrite(fd_.get(), note.data(), note.size(), 0);
  if (written < 0 || static_cast<std::size_t>(written) != note.size()) {
    return {written < 0 ? errno : EIO, std::generic_category()};
  }
  return {};
}

void FileLock::release() noexcept {
  fd_.reset();
}

LockAttempt lock_file(const std::filesystem::path& path, LockMode mode, LockWait wait) {
  LockAttempt attempt;
  for (int retry = 0; retry <= kMaxRelinkRetries; ++retry) {
    const int raw = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, kLockFileMode);
    if (raw < 0) {
      attempt.error = {errno, std::generic_category()};
      return attempt;
    }
    UniqueFd fd(raw);
    int result = 0;
    do {
      result = ::flock(fd.get(), flock_operation(mode, wait));
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      if (errno == EWOULDBLOCK) {
        attempt.outcome = LockOutcome::contended;
        return attempt;
      }
      attempt.error = {errno, std::generic_category()};
      return attempt;
    }
    if (same_file(fd.get(), path)) {
      attempt.outcome = LockOutcome::acquired;
      attempt.lock.fd_ = std::move(fd);
      return attempt;
    }
  }
  attempt.error = std::make_error_code(std::errc::resource_unavailable_try_again);
  return attempt;
}

std::string read_lock_note(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace svp::exec::detail
