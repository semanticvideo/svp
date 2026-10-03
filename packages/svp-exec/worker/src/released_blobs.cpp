#include "svp/exec/worker/released_blobs.hpp"

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
    released_.insert(digest);
  }
}

ReleaseSweep ReleasedBlobs::sweep(CasStore& cas) {
  std::vector<Blake3Digest> candidates;
  {
    const std::lock_guard lock(mutex_);
    for (const Blake3Digest& digest : released_) {
      if (!claims_.contains(digest)) {
        candidates.push_back(digest);
      }
    }
  }
  ReleaseSweep sweep;
  for (const Blake3Digest& digest : candidates) {
    // A session pins a blob before it claims it, so a claim made after the
    // copy above either is seen as a pin here or finds the blob gone (its
    // BLOB_HAVE then reports it missing and it is sent again).
    const CacheResult<BlobRemoval> removal = cas.remove_unpinned(digest);
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
    const std::lock_guard lock(mutex_);
    released_.erase(digest);
  }
  return sweep;
}

std::size_t ReleasedBlobs::pending() const {
  const std::lock_guard lock(mutex_);
  return released_.size();
}

}  // namespace svp::exec::worker
