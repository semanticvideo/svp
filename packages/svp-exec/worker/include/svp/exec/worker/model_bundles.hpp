#pragma once

// Model bundles on workers (plan §3.3 step 5, §4.1 `model_set`). The
// coordinator pushes bundles from its own model cache, after verifying each
// one there; the worker assembles a pushed bundle from blobs in its CAS and
// verifies it against the coordinator's model-lock entry with the same lock
// verification a local build runs (svp::models::verify_lock_against_cache)
// before the bundle is installed. Installed bundles are content-addressed:
//
//   <worker root>/models/<bundle_blake3 hex>/<model_id>/model.svpmodel.json,
//                                                       LICENSE, NOTICE, ...
//
// so each bundle directory is itself a one-bundle model cache that verifies
// against a one-entry lock, and a bundle is never modified in place.

#include "svp/exec/cas_store.hpp"
#include "svp/exec/worker/blob_source.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/transfer_messages.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::exec::worker {

inline constexpr std::string_view kModelManifestFileName = "model.svpmodel.json";

class WorkerModelStore {
 public:
  explicit WorkerModelStore(std::filesystem::path models_dir);

  [[nodiscard]] std::filesystem::path directory_of(const Blake3Digest& bundle_blake3) const;
  [[nodiscard]] bool has(const Blake3Digest& bundle_blake3) const;
  [[nodiscard]] std::vector<Blake3Digest> list() const;

  // Assembles, verifies, and installs one bundle from `cas`. `lock` is a
  // model-lock document holding exactly that bundle's entry. Throws
  // WorkerError(verification) with the lock verification's errors,
  // WorkerError(protocol) for a lock that is not exactly one entry or a blob
  // that never arrived.
  void install_from_cas(const nlohmann::json& lock, const BlobRef& manifest, CasStore& cas) const;

  // Re-runs the lock verification on an installed bundle ("before use").
  void verify(const Blake3Digest& bundle_blake3) const;

 private:
  std::filesystem::path models_dir_;
};

// One bundle the coordinator can push: its lock entry as a one-entry lock,
// its manifest, and every file the lock names.
struct ModelBundleSource {
  std::string model_id;
  std::string model_bundle_id;
  Blake3Digest bundle_blake3{};
  nlohmann::json lock = nlohmann::json::object();
  BlobSource manifest;
  std::vector<BlobSource> files;
};

// Reads `<cache_root>/model-lock.json` and returns the bundles for
// `model_ids` (every locked bundle when empty), each verified in the
// coordinator's cache first. Throws WorkerError(configuration) for an
// unknown model id or a missing lock, WorkerError(verification) when a
// cached bundle does not verify.
[[nodiscard]] std::vector<ModelBundleSource> prepare_model_bundles(
    const std::filesystem::path& cache_root, const std::vector<std::string>& model_ids);

// HELLO's model_set summary for the lock in `cache_root`; nullopt when the
// cache has no lock.
[[nodiscard]] std::optional<ModelSetSummary> model_set_summary(
    const std::filesystem::path& cache_root);

}  // namespace svp::exec::worker
