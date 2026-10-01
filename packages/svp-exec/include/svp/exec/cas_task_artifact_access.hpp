#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/cache_error.hpp"
#include "svp/exec/cas_pin.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/task_artifact_access.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace svp::exec {

// TaskArtifactAccess over the content-addressed cache (plan §4.6): the
// production way task functions see inputs and publish outputs on a node.
//
//   * resolve_inputs() pins each input, re-hashes its blob (RC2 §5.15 rule 5:
//     every read verifies), and hands the task the blob's own read-only path.
//     A pinned blob is re-hashed only when its file changed since this object
//     last verified it (device, inode, size, or modification time differ):
//     CAS blobs are written once (pending -> verify -> rename, read-only) and
//     a pinned one cannot be evicted, so later tasks of a session that read
//     the same input (every frame batch reads the whole source) do not
//     re-hash hundreds of megabytes each, while a blob rewritten in place is
//     still caught.
//     A blob that is missing, corrupt, or of the wrong length is
//     ExecError(unresolved_input), which the attempt runner reports as a
//     retryable failure (bytes may arrive later).
//   * put() is how task functions (and the planner, for source inputs) store
//     bytes: pinned first, so eviction cannot race the write, then written
//     through CasStore's pending -> fsync -> verify -> rename path.
//   * read_outputs() returns each output's verified bytes.
//
// Pinning: every digest this object resolves or stores joins one CasPinSet
// for its lifetime, so eviction (RC2 §20.5.2) cannot remove a live input or
// output while the session runs. Create one per build or worker session and
// keep it alive until the session's results are committed.
//
// RC2 §20.5.2 says cache failures never fail a build. So when the pin set
// cannot be taken, the access runs unpinned; when a put() fails, the bytes
// are kept in memory for this object's lifetime and served by read_outputs()
// from there (such an output cannot be resolved as another task's input,
// which needs a file). Each such failure is recorded in cache_warnings() so
// the caller can report WARN_CACHE_DISABLED. Journal correctness never depends
// on the cache: the journal keeps its own verified copies (RC2 §20.4).
//
// Thread-safe: task threads call it concurrently.
class CasTaskArtifactAccess final : public TaskArtifactAccess {
 public:
  // `pin_holder_id` names the session's pin set ([A-Za-z0-9._-]+, a build or
  // worker session ID); it must not be live in another holder.
  CasTaskArtifactAccess(CasStore store, const std::string& pin_holder_id);

  [[nodiscard]] ResolvedInputs resolve_inputs(const TaskSpec& spec) override;
  [[nodiscard]] std::vector<FramePayload> read_outputs(const TaskResult& result) override;

  // Stores `bytes` content-addressed and pins them. Throws
  // ExecError(invalid_value) for a media_type or role that breaks the wire
  // identifier rules; never throws for cache failures.
  ArtifactRef put(std::span<const std::byte> bytes, std::string media_type, std::string role);

  [[nodiscard]] const CasStore& store() const noexcept { return store_; }
  // False when the pin set could not be taken (see cache_warnings()).
  [[nodiscard]] bool pinned() const;
  [[nodiscard]] std::vector<Blake3Digest> pinned_digests() const;
  [[nodiscard]] std::vector<CacheError> cache_warnings() const;
  // Outputs held in memory because the cache refused them.
  [[nodiscard]] std::uint64_t memory_fallback_bytes() const;

 private:
  void pin(const Blake3Digest& digest);

  CasStore store_;
  mutable std::mutex mutex_;
  std::optional<CasPinSet> pins_;
  std::vector<CacheError> warnings_;
  std::map<Blake3Digest, std::vector<std::byte>> memory_fallback_;
  struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    bool operator==(const FileIdentity&) const = default;
  };
  [[nodiscard]] static std::optional<FileIdentity> file_identity(
      const std::filesystem::path& path);
  // Pinned inputs this object verified, with their file as it was then.
  std::map<Blake3Digest, FileIdentity> verified_inputs_;
  std::uint64_t memory_fallback_bytes_ = 0;
};

}  // namespace svp::exec
