#include "svp/exec/worker/released_blobs.hpp"

#include <utility>

namespace svp::exec::worker {

void ReleasedBlobs::claim(std::string_view coordinator, const Blake3Digest& digest) {
  const std::lock_guard lock(mutex_);
  claims_[digest].emplace(coordinator);
  released_.erase(digest);
}

void ReleasedBlobs::release(std::string_view coordinator,
                            const std::vector<Blake3Digest>& digests) {
  const std::lock_guard lock(mutex_);
  for (const Blake3Digest& digest : digests) {
    const auto claimed = claims_.find(digest);
    if (claimed != claims_.end()) {
      if (const auto mine = claimed->second.find(coordinator); mine != claimed->second.end()) {
        claimed->second.erase(mine);
      }
      if (claimed->second.empty()) {
        claims_.erase(claimed);
      }
    }
    released_.insert_or_assign(digest, ++last_release_);
  }
}

ReleaseSweep ReleasedBlobs::sweep(CasStore& cas) {
  return sweep([&cas](const Blake3Digest& digest) { return cas.remove_unpinned(digest); });
}

ReleaseSweep ReleasedBlobs::sweep(const BlobRemover& remove) {
  std::vector<std::pair<Blake3Digest, std::uint64_t>> candidates;
  {
    const std::lock_guard lock(mutex_);
    for (const auto& [digest, release] : released_) {
      if (!claims_.contains(digest)) {
        candidates.emplace_back(digest, release);
      }
    }
  }
  ReleaseSweep sweep;
  for (const auto& [digest, release] : candidates) {
    // A session pins a blob before it claims it, so a claim made after the
    // copy above either is seen as a pin here or finds the blob gone (its
    // BLOB_HAVE then reports it missing and it is sent again).
    const CacheResult<BlobRemoval> removal = remove(digest);
    if (!removal) {
      continue;
    }
    if (removal.value() == BlobRemoval::pinned) {
      ++sweep.pinned_blobs;
      continue;
    }
    if (removal.value() == BlobRemoval::removed) {
      ++sweep.removed_blobs;
    }
    // Forget the blob only if no session claimed or released it since the
    // copy: a claim drops the marker and a release stamps a newer one, so a
    // blob sent again and released during this sweep stays for the next.
    const std::lock_guard lock(mutex_);
    if (const auto marked = released_.find(digest);
        marked != released_.end() && marked->second == release && !claims_.contains(digest)) {
      released_.erase(marked);
    }
  }
  return sweep;
}

std::size_t ReleasedBlobs::pending() const {
  const std::lock_guard lock(mutex_);
  return released_.size();
}

}  // namespace svp::exec::worker
