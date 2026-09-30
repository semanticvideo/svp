#pragma once

// Advisory whole-file locks (flock) that survive only as long as the owning
// open file description. A crashed process therefore never leaves a stale
// lock behind, which is what lets the journal and the cache pin registry tell
// live holders from dead ones.

#include "durable_io.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace svp::exec::detail {

enum class LockMode { shared, exclusive };
enum class LockWait { try_once, block };

struct LockAttempt;

class FileLock {
 public:
  FileLock() = default;

  [[nodiscard]] bool held() const noexcept { return fd_.valid(); }

  // Replaces the lock file's content with `note` (for example the owner's
  // PID) so a contender can report who holds it.
  [[nodiscard]] std::error_code write_note(std::string_view note) const;

  void release() noexcept;

 private:
  friend LockAttempt lock_file(const std::filesystem::path&, LockMode, LockWait);

  UniqueFd fd_;
};

enum class LockOutcome { acquired, contended, failed };

struct LockAttempt {
  LockOutcome outcome = LockOutcome::failed;
  FileLock lock;
  std::error_code error;
};

// Opens (creating if needed) and locks `path`. A holder that deletes its lock
// file while another process waits would leave the waiter locking an
// unlinked inode; after acquiring, the path is re-checked and the attempt
// retried, so an acquired lock always belongs to the file at `path`.
[[nodiscard]] LockAttempt lock_file(const std::filesystem::path& path, LockMode mode,
                                    LockWait wait);

// Reads a lock file's note, empty when unreadable.
[[nodiscard]] std::string read_lock_note(const std::filesystem::path& path);

}  // namespace svp::exec::detail
