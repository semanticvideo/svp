#pragma once

#include "svp/exec/cas_store.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/worker/transfer_messages.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace svp::exec::worker {

// Verifies a runtime directory (worker_layout.hpp layout): its manifest
// decodes, its runtime_id equals `expected` when given, and every file
// matches the manifest's size and BLAKE3. Returns the runtime_id. Throws
// WorkerError(verification) naming the first problem, WorkerError(io) when
// the manifest cannot be read.
Blake3Digest verify_runtime_directory(const std::filesystem::path& runtime_dir,
                                      const std::optional<Blake3Digest>& expected);

// The runtimes a worker holds: <runtimes>/<runtime hex>/ (plan §3.2: "the
// agent runs only the runtime delivered by its paired coordinator ...,
// verified by BLAKE3 before execution").
class WorkerRuntimeStore {
 public:
  explicit WorkerRuntimeStore(std::filesystem::path runtimes_dir);

  [[nodiscard]] std::filesystem::path directory_of(const Blake3Digest& runtime_id) const;
  // Installed and its manifest names this runtime_id (files not hashed).
  [[nodiscard]] bool has(const Blake3Digest& runtime_id) const;
  [[nodiscard]] std::vector<Blake3Digest> list() const;
  // Full verification (see verify_runtime_directory).
  void verify(const Blake3Digest& runtime_id) const;

  // Assembles the runtime from blobs already in `cas`: the manifest (which
  // must hash to `runtime_id`), components.json when given, and every file
  // the manifest names. Verifies the result and renames it into place; a
  // concurrent install of the same runtime is accepted when it verifies.
  // Throws WorkerError(verification, io, protocol).
  void install_from_cas(const Blake3Digest& runtime_id, const BlobRef& manifest,
                        const std::optional<BlobRef>& components, CasStore& cas) const;

 private:
  std::filesystem::path runtimes_dir_;
};

// Reads a whole blob from the CAS (verified). Throws WorkerError(protocol)
// when it is missing and WorkerError(verification) when it is corrupt.
[[nodiscard]] std::vector<std::byte> read_cas_blob(CasStore& cas, const BlobRef& blob);
// Copies a verified blob to `target` (created with `mode`).
void copy_cas_blob(CasStore& cas, const BlobRef& blob, const std::filesystem::path& target,
                   std::filesystem::perms mode);

}  // namespace svp::exec::worker
