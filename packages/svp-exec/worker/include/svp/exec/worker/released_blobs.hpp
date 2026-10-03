#pragma once

// Blobs coordinators released on this worker (BLOB_RELEASE, protocol 1.2,
// transfer_messages.hpp): a finished build's source, which would otherwise
// stay in the worker's CAS until its size budget evicts it.
//
// Each coordinator claims the blobs its sessions declare (BLOB_HAVE) and
// gives its claim up when it releases them. A released blob is deleted once
// no coordinator claims it and no live holder pins it
// (CasStore::remove_unpinned): holders are each session's agent (the blobs
// it declared) and each session process (the inputs its tasks resolved). So
// another coordinator building the same video keeps it, also between its
// sessions, until it releases it too; and a blob the releasing session's own
// process has not let go of yet is tried again when a session ends.
//
// Held in memory by the worker agent. A claim that is never given up (a
// coordinator of protocol 1.1, or one that crashed or lost the connection
// before it released) keeps the blob, and a release the agent forgets (it
// restarts) leaves it: such blobs stay until the cache's size budget evicts
// them, as before protocol 1.2. Claims are one entry per (blob, coordinator)
// a session declared, so they grow with the distinct blobs coordinators
// send, not with time. Thread-safe.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cas_store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

struct ReleaseSweep {
  std::uint64_t removed_blobs = 0;
  // Released blobs a live holder still pins, kept for a later sweep.
  std::uint64_t pinned_blobs = 0;
};

class ReleasedBlobs {
 public:
  // Deletes one blob unless a live holder pins it (CasStore::remove_unpinned).
  using BlobRemover = std::function<CacheResult<BlobRemoval>(const Blake3Digest&)>;

  // `coordinator` declared it needs `digest`; an earlier release of it no
  // longer deletes it while this claim stands.
  void claim(std::string_view coordinator, const Blake3Digest& digest);
  // `coordinator` no longer needs `digests`: its claims end and each blob
  // is deleted by a sweep once nothing else claims or pins it.
  void release(std::string_view coordinator, const std::vector<Blake3Digest>& digests);
  // Deletes every released, unclaimed blob no live holder pins. A blob that
  // is gone (deleted now, or already absent) is forgotten; one that is
  // pinned, or whose removal failed, stays released for the next sweep.
  //
  // Removal runs without the lock, so sessions claim and release while a
  // sweep runs. A blob released again during the sweep (a session found it
  // gone, sent it again, and released it) stays released for the next one.
  ReleaseSweep sweep(CasStore& cas);
  // The same sweep with `remove` in place of the CAS.
  ReleaseSweep sweep(const BlobRemover& remove);
  // Released blobs not deleted yet.
  [[nodiscard]] std::size_t pending() const;

 private:
  mutable std::mutex mutex_;
  std::map<Blake3Digest, std::set<std::string, std::less<>>> claims_;
  // Each released blob with the release that marked it: a sweep forgets a
  // blob only when the release it saw is still the latest one.
  std::map<Blake3Digest, std::uint64_t> released_;
  std::uint64_t last_release_ = 0;
};

}  // namespace svp::exec::worker
