#pragma once

// POSIX file primitives for durable, verifiable writes (RC2 §20.4: written to
// a pending path, fsynced where supported, verified by BLAKE3, then atomically
// moved). Errors are std::error_code so the cache can stay non-throwing.

#include "svp/exec/blake3_digest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>

namespace svp::exec::detail {

// Streaming reads and copies move data in chunks of this size. 1 MiB keeps
// syscall overhead negligible against BLAKE3 throughput while bounding the
// buffer each concurrent hash or copy holds.
inline constexpr std::size_t kFileStreamChunkBytes = 1024U * 1024U;

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  UniqueFd(UniqueFd&& other) noexcept;
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  ~UniqueFd();

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  void reset() noexcept;

 private:
  int fd_ = -1;
};

struct FileDigest {
  Blake3Digest digest{};
  std::uint64_t bytes = 0;
};

// Flushes file data to stable storage. On Apple platforms fsync() only reaches
// the drive's volatile cache, so F_FULLFSYNC is used when the filesystem
// supports it.
[[nodiscard]] std::error_code sync_fd(int fd) noexcept;

// Persists a rename or unlink inside `directory`.
[[nodiscard]] std::error_code sync_directory(const std::filesystem::path& directory) noexcept;

// Streams `path` through BLAKE3.
[[nodiscard]] std::error_code hash_file(const std::filesystem::path& path, FileDigest& out);

// Creates `path` exclusively (fails if it exists), writes `bytes`, syncs.
[[nodiscard]] std::error_code write_new_file_synced(const std::filesystem::path& path,
                                                    std::span<const std::byte> bytes);

// Creates `destination` exclusively and copies `source` into it while hashing
// the source stream into `source_digest`, then syncs.
[[nodiscard]] std::error_code copy_to_new_file_synced(
    const std::filesystem::path& source, const std::filesystem::path& destination,
    FileDigest& source_digest);

}  // namespace svp::exec::detail
