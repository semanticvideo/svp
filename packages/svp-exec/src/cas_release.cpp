#include "svp/exec/cas_store.hpp"

#include "cache_error_mapping.hpp"
#include "cas_layout.hpp"
#include "cas_pin_registry.hpp"
#include "file_lock.hpp"

#include <set>

namespace svp::exec {

CacheResult<BlobRemoval> CasStore::remove_unpinned(const Blake3Digest& digest) {
  const detail::CasPaths paths = detail::cas_paths(root_);
  // Exclusive, like evict(): no pin can be added while the pins are read and
  // the blob is removed.
  detail::LockAttempt eviction =
      detail::lock_file(paths.eviction_lock, detail::LockMode::exclusive, detail::LockWait::block);
  if (eviction.outcome != detail::LockOutcome::acquired) {
    return detail::cache_error(eviction.error, "cache remove: eviction lock");
  }
  std::set<Blake3Digest> pinned;
  if (const auto error = detail::read_live_pins(root_, pinned)) {
    return detail::cache_error(error, "cache remove: read pins");
  }
  if (pinned.contains(digest)) {
    return BlobRemoval::pinned;
  }
  std::error_code error;
  if (std::filesystem::remove(detail::cas_blob_path(root_, digest), error)) {
    return BlobRemoval::removed;
  }
  if (error) {
    return detail::cache_error(error, "cache remove: blob " + blake3_hex(digest));
  }
  return BlobRemoval::absent;
}

}  // namespace svp::exec
