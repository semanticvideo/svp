#include "svp/exec/cas_store.hpp"

#include "cache_error_mapping.hpp"
#include "cas_layout.hpp"
#include "cas_pin_registry.hpp"
#include "file_lock.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace svp::exec {
namespace {

namespace fs = std::filesystem;
using detail::cache_error;

// Deletes pending files abandoned by crashed writers (CachePolicy docs).
std::uint64_t remove_stale_pending(const fs::path& pending_dir, fs::file_time_type now,
                                   std::chrono::seconds stale_age) {
  std::uint64_t removed = 0;
  std::error_code error;
  for (fs::directory_iterator entry(pending_dir, error), end; !error && entry != end;
       entry.increment(error)) {
    std::error_code entry_error;
    const fs::file_time_type written = entry->last_write_time(entry_error);
    if (entry_error || !entry->is_regular_file(entry_error) || now - written < stale_age) {
      continue;
    }
    if (fs::remove(entry->path(), entry_error)) {
      ++removed;
    }
  }
  return removed;
}

}  // namespace

CacheResult<EvictionReport> CasStore::evict() {
  const detail::CasPaths paths = detail::cas_paths(root_);
  // Exclusive against other evictors and against CasPinSet::add (shared), so
  // the pin set read below cannot change until the lock is released.
  detail::LockAttempt eviction =
      detail::lock_file(paths.eviction_lock, detail::LockMode::exclusive, detail::LockWait::try_once);
  if (eviction.outcome == detail::LockOutcome::contended) {
    return cache_error(CacheErrorCode::busy, "cache eviction already running");
  }
  if (eviction.outcome == detail::LockOutcome::failed) {
    return cache_error(eviction.error, "cache eviction lock");
  }

  std::set<Blake3Digest> pinned;
  if (const auto error = detail::read_live_pins(root_, pinned)) {
    return cache_error(error, "cache eviction: read pins");
  }

  EvictionReport report;
  const fs::file_time_type now = options_.now();
  report.stale_pending_removed =
      remove_stale_pending(paths.pending, now, options_.policy.stale_pending_age);

  std::error_code error;
  std::vector<detail::CasBlobEntry> blobs = detail::list_cas_blobs(root_, error);
  if (error) {
    return cache_error(error, "cache eviction: list blobs");
  }
  std::uint64_t total = 0;
  for (const detail::CasBlobEntry& blob : blobs) {
    total += blob.bytes;
  }
  // Oldest first; the digest breaks ties so the order is deterministic.
  std::sort(blobs.begin(), blobs.end(), [](const auto& left, const auto& right) {
    return std::tie(left.last_used, left.digest) < std::tie(right.last_used, right.digest);
  });
  for (const detail::CasBlobEntry& blob : blobs) {
    if (total <= options_.policy.max_bytes) {
      break;
    }
    if (pinned.contains(blob.digest)) {
      ++report.pinned_blobs_kept;
      continue;
    }
    std::error_code remove_error;
    if (fs::remove(blob.path, remove_error)) {
      ++report.evicted_blobs;
      report.evicted_bytes += blob.bytes;
      total -= blob.bytes;
    }
  }
  report.remaining_bytes = total;
  return report;
}

}  // namespace svp::exec
