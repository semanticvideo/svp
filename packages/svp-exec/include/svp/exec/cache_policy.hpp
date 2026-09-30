#pragma once

#include <chrono>
#include <cstdint>

namespace svp::exec {

// RC2 §20.5.2 default cache policy: `maximum_size: 50 GiB`, LRU eviction.
inline constexpr std::uint64_t kDefaultCacheMaxBytes =
    50ULL * 1024ULL * 1024ULL * 1024ULL;

// A pending file exists only while one put() writes, syncs, and verifies a
// single blob, which takes seconds to minutes even for the largest source
// media. A pending file untouched for a day belongs to a writer that crashed
// or was killed, so eviction may delete it without racing a live writer.
inline constexpr std::chrono::hours kDefaultStalePendingAge{24};

struct CachePolicy {
  // Eviction prunes least-recently-used, unpinned blobs until the stored blob
  // bytes are at or below this limit.
  std::uint64_t max_bytes = kDefaultCacheMaxBytes;
  // Eviction deletes pending files whose last write is at least this old.
  std::chrono::seconds stale_pending_age = kDefaultStalePendingAge;
};

}  // namespace svp::exec
