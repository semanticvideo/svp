#include "verified_blob.hpp"

#include <atomic>
#include <random>
#include <unistd.h>

namespace svp::exec::detail {
namespace {

namespace fs = std::filesystem;

constexpr fs::perms kPublishedBlobPerms =
    fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read;

StageResult verify_staged(StagedBlob blob, const Blake3Digest& written,
                          const std::optional<Blake3Digest>& expected) {
  StageResult result;
  FileDigest on_disk;
  if (const auto error = hash_file(blob.pending_path, on_disk)) {
    discard_staged_blob(blob);
    result.error = error;
    return result;
  }
  if (on_disk.digest != written || on_disk.bytes != blob.bytes ||
      (expected.has_value() && *expected != written)) {
    discard_staged_blob(blob);
    result.status = StageStatus::digest_mismatch;
    return result;
  }
  blob.digest = written;
  result.status = StageStatus::staged;
  result.blob = std::move(blob);
  return result;
}

}  // namespace

std::string pending_file_name() {
  static std::atomic<std::uint64_t> counter{0};
  static const std::uint64_t process_nonce = [] {
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32U) ^ device();
  }();
  return std::to_string(::getpid()) + "-" + std::to_string(process_nonce) + "-" +
         std::to_string(counter.fetch_add(1)) + std::string(kPendingFileSuffix);
}

StageResult stage_blob_bytes(const fs::path& pending_dir, std::span<const std::byte> bytes,
                             const std::optional<Blake3Digest>& expected) {
  StagedBlob blob{.pending_path = pending_dir / pending_file_name(), .bytes = bytes.size()};
  if (const auto error = write_new_file_synced(blob.pending_path, bytes)) {
    discard_staged_blob(blob);
    return StageResult{.error = error};
  }
  return verify_staged(std::move(blob), blake3_digest(bytes), expected);
}

StageResult stage_blob_file(const fs::path& pending_dir, const fs::path& source,
                            const std::optional<Blake3Digest>& expected) {
  StagedBlob blob{.pending_path = pending_dir / pending_file_name()};
  FileDigest source_digest;
  if (const auto error = copy_to_new_file_synced(source, blob.pending_path, source_digest)) {
    discard_staged_blob(blob);
    return StageResult{.error = error};
  }
  blob.bytes = source_digest.bytes;
  return verify_staged(std::move(blob), source_digest.digest, expected);
}

std::error_code publish_staged_blob(const StagedBlob& blob, const fs::path& final_path) {
  std::error_code error;
  fs::permissions(blob.pending_path, kPublishedBlobPerms, fs::perm_options::replace, error);
  if (error) {
    return error;
  }
  fs::create_directories(final_path.parent_path(), error);
  if (error) {
    return error;
  }
  fs::rename(blob.pending_path, final_path, error);
  if (error) {
    return error;
  }
  return sync_directory(final_path.parent_path());
}

void discard_staged_blob(const StagedBlob& blob) noexcept {
  std::error_code ignored;
  fs::remove(blob.pending_path, ignored);
}

BlobCheck check_blob(const fs::path& path, const Blake3Digest& expected,
                     std::optional<std::uint64_t> expected_bytes, std::error_code& error) {
  error.clear();
  const fs::file_status status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) {
    error.clear();
    return BlobCheck::missing;
  }
  if (error) {
    return BlobCheck::io_error;
  }
  if (!fs::is_regular_file(status)) {
    // A symlink or directory in a blob slot is never trusted.
    return BlobCheck::not_regular_file;
  }
  if (expected_bytes.has_value()) {
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
      return BlobCheck::io_error;
    }
    if (size != *expected_bytes) {
      return BlobCheck::size_mismatch;
    }
  }
  FileDigest actual;
  if ((error = hash_file(path, actual))) {
    return BlobCheck::io_error;
  }
  return actual.digest == expected ? BlobCheck::valid : BlobCheck::digest_mismatch;
}

}  // namespace svp::exec::detail
