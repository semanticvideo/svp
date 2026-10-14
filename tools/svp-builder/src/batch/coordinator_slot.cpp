#include "batch/coordinator_slot.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace svp::builder::batch {
namespace {

// How often a build waiting for the slot looks again; small next to any
// build, and far inside the builder's interrupt grace period.
constexpr std::chrono::milliseconds kSlotPollInterval{250};

int open_lock_file(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::create_directories(path.parent_path(), ignored);
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error("cannot open the coordinator slot " + path.string() + ": " +
                             std::strerror(errno));
  }
  return fd;
}

}  // namespace

std::filesystem::path default_coordinator_slot_path() {
  if (const char* configured = std::getenv("SVP_COORDINATOR_SLOT");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home != nullptr ? home : "") / "Library" / "Application Support" /
         "SVP" / "Coordinator" / "slot.lock";
}

std::optional<CoordinatorSlot> CoordinatorSlot::try_take(const std::filesystem::path& path) {
  const int fd = open_lock_file(path);
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
    return CoordinatorSlot(fd);
  }
  ::close(fd);
  return std::nullopt;
}

std::optional<CoordinatorSlot> CoordinatorSlot::take(
    const std::filesystem::path& path, const svp::exec::CancellationToken* cancellation) {
  while (cancellation == nullptr || !cancellation->requested()) {
    if (std::optional<CoordinatorSlot> slot = try_take(path)) {
      return slot;
    }
    std::this_thread::sleep_for(kSlotPollInterval);
  }
  return std::nullopt;
}

CoordinatorSlot::CoordinatorSlot(CoordinatorSlot&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

CoordinatorSlot& CoordinatorSlot::operator=(CoordinatorSlot&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

CoordinatorSlot::~CoordinatorSlot() {
  if (fd_ >= 0) {
    ::close(fd_);  // closing the descriptor releases the flock
  }
}

}  // namespace svp::builder::batch
