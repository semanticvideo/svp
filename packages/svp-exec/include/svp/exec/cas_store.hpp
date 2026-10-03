#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cache_error.hpp"
#include "svp/exec/cache_policy.hpp"
#include "svp/exec/cas_pin.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace svp::exec {

// Clock that stamps blob recency. Injectable so tests can order hits exactly.
using CacheClock = std::function<std::filesystem::file_time_type()>;

struct CasStoreOptions {
  CachePolicy policy{};
  // Defaults to std::filesystem::file_time_type::clock::now.
  CacheClock now;
};

struct CacheUsage {
  std::uint64_t blob_count = 0;
  std::uint64_t total_bytes = 0;
};

struct EvictionReport {
  std::uint64_t evicted_blobs = 0;
  std::uint64_t evicted_bytes = 0;
  // Pinned blobs that would otherwise have been evicted.
  std::uint64_t pinned_blobs_kept = 0;
  std::uint64_t stale_pending_removed = 0;
  // Blob bytes left after eviction. Above policy.max_bytes only when pinned
  // blobs alone exceed the limit.
  std::uint64_t remaining_bytes = 0;
};

// What CasStore::remove_unpinned did with one blob.
enum class BlobRemoval {
  removed,
  // A live holder pins it; it stays.
  pinned,
  // Not stored (never was, or already removed).
  absent,
};

// Global content-addressable cache (RC2 §20.3, plan §4.6).
//
// Layout under the root:
//   blobs/b3/<first 2 hex>/<remaining 62 hex>   read-only blob files
//   pending/                                    in-flight writes, never read
//   pins/                                       see CasPinSet
//   locks/eviction.lock                         serializes eviction and pins
//
// Every write goes to a unique pending file, is fsynced, re-read and verified
// by BLAKE3, and only then renamed onto its digest path. Every read verifies
// the digest (RC2 §5.15 rule 5). Recency for LRU eviction is the blob file's
// modification time, refreshed on every put() or verified read.
//
// No operation throws for cache failures: each returns a CacheError so the
// caller can fall back to direct compute (RC2 §20.5.2).
class CasStore {
 public:
  // Creates the layout under `root` and proves it writable.
  static CacheResult<CasStore> at(std::filesystem::path root,
                                  CasStoreOptions options = {});
  // at(default_cache_root()), or `unavailable` when no root resolves.
  static CacheResult<CasStore> at_default_root(CasStoreOptions options = {});

  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] const CachePolicy& policy() const noexcept;

  // Idempotent: storing bytes that are already present (and still verify)
  // refreshes recency and returns the same digest.
  CacheResult<Blake3Digest> put(std::span<const std::byte> bytes);
  CacheResult<Blake3Digest> put_file(const std::filesystem::path& source);

  // Presence only; does not hash.
  [[nodiscard]] bool has(const Blake3Digest& digest) const;

  // Verified read. A blob that fails verification is removed and reported as
  // `corrupt`.
  CacheResult<std::vector<std::byte>> get(const Blake3Digest& digest);
  // Verifies, then opens the blob for binary reading.
  CacheResult<std::ifstream> open(const Blake3Digest& digest);
  // Re-hashes the blob. `corrupt` (and removal) on mismatch.
  CacheStatus verify(const Blake3Digest& digest);

  CacheResult<CacheUsage> usage() const;
  // Deletes stale pending files, then least-recently-used unpinned blobs
  // until usage is at or below policy.max_bytes. `busy` when another process
  // is evicting.
  CacheResult<EvictionReport> evict();
  // Deletes one blob now, unless a live holder pins it (the same rule
  // evict() follows, regardless of policy.max_bytes). Serialized against
  // evict() and CasPinSet::add like eviction is: it waits for a running
  // eviction, and a pin added concurrently either lands first (the blob
  // stays) or finds the blob gone (add() reports not_found).
  CacheResult<BlobRemoval> remove_unpinned(const Blake3Digest& digest);

  // `holder_id` must be [A-Za-z0-9._-]+ (a build or worker session ID).
  // `busy` when a live holder already uses that ID.
  CacheResult<CasPinSet> pin_set(const std::string& holder_id);

 private:
  CasStore(std::filesystem::path root, CasStoreOptions options);

  std::filesystem::path root_;
  CasStoreOptions options_;
};

}  // namespace svp::exec
