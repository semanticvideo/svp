#include "svp/exec/cas_store.hpp"

#include "cache_error_mapping.hpp"
#include "cas_layout.hpp"
#include "cas_pin_registry.hpp"
#include "durable_io.hpp"
#include "record_identifiers.hpp"
#include "svp/exec/cache_root.hpp"
#include "verified_blob.hpp"

#include <fstream>

namespace svp::exec {
namespace {

namespace fs = std::filesystem;
using detail::cache_error;

CacheError corrupt_blob(const fs::path& path, const Blake3Digest& digest) {
  std::error_code ignored;
  fs::remove(path, ignored);
  return cache_error(CacheErrorCode::corrupt,
                     "cache blob " + blake3_hex(digest) + " failed verification and was removed");
}

CacheError stage_failure(const detail::StageResult& staged, std::string_view context) {
  if (staged.status == detail::StageStatus::digest_mismatch) {
    return cache_error(CacheErrorCode::corrupt,
                       std::string(context) + ": bytes read back from the pending file differ");
  }
  return cache_error(staged.error, context);
}

std::error_code read_file(const fs::path& path, std::vector<std::byte>& bytes) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error) {
    return error;
  }
  bytes.resize(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  // A short read means the file changed underneath us; the digest check
  // would catch it too, but report it as I/O rather than corruption.
  if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    return std::make_error_code(std::errc::io_error);
  }
  return {};
}

}  // namespace

CasStore::CasStore(fs::path root, CasStoreOptions options)
    : root_(std::move(root)), options_(std::move(options)) {
  if (!options_.now) {
    options_.now = [] { return fs::file_time_type::clock::now(); };
  }
}

CacheResult<CasStore> CasStore::at(fs::path root, CasStoreOptions options) {
  if (root.empty()) {
    return cache_error(CacheErrorCode::invalid_argument, "cache root is empty");
  }
  const detail::CasPaths paths = detail::cas_paths(root);
  for (const fs::path& directory : {paths.blobs, paths.pending, paths.pins, paths.locks}) {
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
      return cache_error(error, "cache root " + root.string());
    }
  }
  // create_directories succeeds on an existing read-only tree, so prove the
  // root writable before any caller relies on it.
  const fs::path probe = paths.pending / detail::pending_file_name();
  const std::error_code probe_error = detail::write_new_file_synced(probe, {});
  std::error_code ignored;
  fs::remove(probe, ignored);
  if (probe_error) {
    return cache_error(probe_error, "cache root " + root.string() + " is not writable");
  }
  return CasStore(std::move(root), std::move(options));
}

CacheResult<CasStore> CasStore::at_default_root(CasStoreOptions options) {
  const auto root = default_cache_root();
  if (!root) {
    return cache_error(CacheErrorCode::unavailable,
                       "no cache root: set SVP_CACHE_DIR or the platform home variables");
  }
  return at(*root, std::move(options));
}

const fs::path& CasStore::root() const noexcept {
  return root_;
}

const CachePolicy& CasStore::policy() const noexcept {
  return options_.policy;
}

namespace {

// Refreshes LRU recency. A failure only makes the blob look older.
void touch(const fs::path& path, const CacheClock& now) {
  std::error_code ignored;
  fs::last_write_time(path, now(), ignored);
}

// Publishes a verified staged blob unless an intact copy is already stored.
CacheResult<Blake3Digest> publish(const fs::path& root, const detail::StagedBlob& staged,
                                  const CacheClock& now) {
  const fs::path final_path = detail::cas_blob_path(root, staged.digest);
  std::error_code check_error;
  if (detail::check_blob(final_path, staged.digest, staged.bytes, check_error) ==
      detail::BlobCheck::valid) {
    detail::discard_staged_blob(staged);
    touch(final_path, now);
    return staged.digest;
  }
  if (const auto error = detail::publish_staged_blob(staged, final_path)) {
    detail::discard_staged_blob(staged);
    return cache_error(error, "cache put " + blake3_hex(staged.digest));
  }
  touch(final_path, now);
  return staged.digest;
}

}  // namespace

CacheResult<Blake3Digest> CasStore::put(std::span<const std::byte> bytes) {
  const Blake3Digest digest = blake3_digest(bytes);
  const fs::path final_path = detail::cas_blob_path(root_, digest);
  std::error_code check_error;
  if (detail::check_blob(final_path, digest, bytes.size(), check_error) ==
      detail::BlobCheck::valid) {
    touch(final_path, options_.now);
    return digest;
  }
  const detail::StageResult staged =
      detail::stage_blob_bytes(detail::cas_paths(root_).pending, bytes, digest);
  if (staged.status != detail::StageStatus::staged) {
    return stage_failure(staged, "cache put");
  }
  return publish(root_, staged.blob, options_.now);
}

CacheResult<Blake3Digest> CasStore::put_file(const fs::path& source) {
  const detail::StageResult staged =
      detail::stage_blob_file(detail::cas_paths(root_).pending, source, std::nullopt);
  if (staged.status != detail::StageStatus::staged) {
    return stage_failure(staged, "cache put_file " + source.string());
  }
  return publish(root_, staged.blob, options_.now);
}

bool CasStore::has(const Blake3Digest& digest) const {
  std::error_code error;
  return fs::is_regular_file(detail::cas_blob_path(root_, digest), error);
}

CacheResult<std::vector<std::byte>> CasStore::get(const Blake3Digest& digest) {
  const fs::path path = detail::cas_blob_path(root_, digest);
  std::error_code error;
  if (!fs::is_regular_file(path, error)) {
    return cache_error(CacheErrorCode::not_found, "cache blob " + blake3_hex(digest));
  }
  std::vector<std::byte> bytes;
  if (const auto read_error = read_file(path, bytes)) {
    return cache_error(read_error, "cache read " + blake3_hex(digest));
  }
  if (blake3_digest(bytes) != digest) {
    return corrupt_blob(path, digest);
  }
  touch(path, options_.now);
  return CacheResult<std::vector<std::byte>>(std::move(bytes));
}

CacheResult<std::ifstream> CasStore::open(const Blake3Digest& digest) {
  const CacheStatus verified = verify(digest);
  if (!verified) {
    return verified.error();
  }
  const fs::path path = detail::cas_blob_path(root_, digest);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return cache_error(CacheErrorCode::io_error, "cache open " + blake3_hex(digest));
  }
  touch(path, options_.now);
  return CacheResult<std::ifstream>(std::move(stream));
}

CacheStatus CasStore::verify(const Blake3Digest& digest) {
  const fs::path path = detail::cas_blob_path(root_, digest);
  std::error_code error;
  switch (detail::check_blob(path, digest, std::nullopt, error)) {
    case detail::BlobCheck::valid:
      return CacheOk{};
    case detail::BlobCheck::missing:
      return cache_error(CacheErrorCode::not_found, "cache blob " + blake3_hex(digest));
    case detail::BlobCheck::io_error:
      return cache_error(error, "cache verify " + blake3_hex(digest));
    case detail::BlobCheck::not_regular_file:
    case detail::BlobCheck::size_mismatch:
    case detail::BlobCheck::digest_mismatch:
      break;
  }
  return corrupt_blob(path, digest);
}

CacheResult<CacheUsage> CasStore::usage() const {
  std::error_code error;
  const std::vector<detail::CasBlobEntry> blobs = detail::list_cas_blobs(root_, error);
  if (error) {
    return cache_error(error, "cache usage");
  }
  CacheUsage usage;
  for (const detail::CasBlobEntry& blob : blobs) {
    ++usage.blob_count;
    usage.total_bytes += blob.bytes;
  }
  return usage;
}

CacheResult<CasPinSet> CasStore::pin_set(const std::string& holder_id) {
  if (!detail::is_record_identifier(holder_id)) {
    return cache_error(CacheErrorCode::invalid_argument,
                       "cache pin holder id must be [A-Za-z0-9._-]+: " + holder_id);
  }
  const fs::path lock_path =
      detail::cas_paths(root_).pins / (holder_id + std::string(detail::kPinLockSuffix));
  detail::LockAttempt attempt =
      detail::lock_file(lock_path, detail::LockMode::exclusive, detail::LockWait::try_once);
  if (attempt.outcome == detail::LockOutcome::contended) {
    return cache_error(CacheErrorCode::busy, "cache pin holder " + holder_id + " is already live");
  }
  if (attempt.outcome == detail::LockOutcome::failed) {
    return cache_error(attempt.error, "cache pin holder " + holder_id);
  }
  auto state = std::make_unique<detail::CasPinSetState>();
  state->root = root_;
  state->holder_id = holder_id;
  state->holder_lock = std::move(attempt.lock);
  return CasPinSet(std::move(state));
}

}  // namespace svp::exec
