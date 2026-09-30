#pragma once

// The one write path shared by the cache and the journal (RC2 §20.4): write
// to a unique pending file, fsync, re-read and verify by BLAKE3, then
// atomically rename onto the final path and sync the directory.

#include "durable_io.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace svp::exec::detail {

// Suffix of in-flight files. Readers never trust anything carrying it.
inline constexpr std::string_view kPendingFileSuffix = ".pending";

struct StagedBlob {
  std::filesystem::path pending_path;
  Blake3Digest digest{};
  std::uint64_t bytes = 0;
};

enum class StageStatus {
  staged,
  io_error,
  // The bytes read back from the pending file differ from what was written,
  // or from the digest the caller expected.
  digest_mismatch,
};

struct StageResult {
  StageStatus status = StageStatus::io_error;
  StagedBlob blob;
  std::error_code error;
};

// A pending file name unique across processes and threads.
[[nodiscard]] std::string pending_file_name();

// `expected`, when given, must equal the content digest.
[[nodiscard]] StageResult stage_blob_bytes(const std::filesystem::path& pending_dir,
                                           std::span<const std::byte> bytes,
                                           const std::optional<Blake3Digest>& expected);
[[nodiscard]] StageResult stage_blob_file(const std::filesystem::path& pending_dir,
                                          const std::filesystem::path& source,
                                          const std::optional<Blake3Digest>& expected);

// Makes the staged file read-only, creates the final parent directory, renames
// the staged file onto `final_path` (replacing any file there), and syncs the
// parent directory.
[[nodiscard]] std::error_code publish_staged_blob(const StagedBlob& blob,
                                                  const std::filesystem::path& final_path);

void discard_staged_blob(const StagedBlob& blob) noexcept;

enum class BlobCheck {
  valid,
  missing,
  not_regular_file,
  size_mismatch,
  digest_mismatch,
  io_error,
};

// Re-hashes a stored blob. `expected_bytes`, when given, is checked first so a
// truncated file is reported without hashing.
[[nodiscard]] BlobCheck check_blob(const std::filesystem::path& path,
                                   const Blake3Digest& expected,
                                   std::optional<std::uint64_t> expected_bytes,
                                   std::error_code& error);

}  // namespace svp::exec::detail
