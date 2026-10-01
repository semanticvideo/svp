#include "svp/exec/worker/model_bundles.hpp"

#include "svp/exec/worker/runtime_store.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/model_lock.hpp"
#include "svp/models/verification.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace svp::exec::worker {
namespace {

std::string first_error(const svp::models::VerificationReport& report) {
  std::string text;
  for (const svp::models::VerificationIssue& issue : report.issues) {
    if (issue.severity == svp::models::VerificationSeverity::error) {
      text += (text.empty() ? "" : "; ") + issue.message;
    }
  }
  return text;
}

svp::models::ModelLock single_entry_lock(const nlohmann::json& lock) {
  svp::models::ModelLock parsed;
  try {
    parsed = svp::models::parse_model_lock(lock, "pushed model lock");
  } catch (const std::exception& error) {
    throw WorkerError(WorkerErrorCode::protocol,
                      std::string("pushed model lock is invalid: ") + error.what());
  }
  if (parsed.models.size() != 1) {
    throw WorkerError(WorkerErrorCode::protocol,
                      "a pushed model lock must hold exactly one bundle");
  }
  return parsed;
}

Blake3Digest bundle_digest(const svp::models::ModelLockEntry& entry) {
  const std::optional<Blake3Digest> digest = parse_blake3_hex(entry.bundle_blake3.hex_value());
  if (!digest) {
    throw WorkerError(WorkerErrorCode::protocol,
                      "bundle_blake3 of " + entry.model_bundle_id + " is not a BLAKE3 digest");
  }
  return *digest;
}

bool safe_relative(const std::string& path) {
  const std::filesystem::path relative(path);
  if (path.empty() || relative.is_absolute()) {
    return false;
  }
  for (const auto& part : relative) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  return true;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

// The cached bundle a lock entry names: <cache>/<model_id> in the layout
// svp-models-tool installs, otherwise whichever directory directly under the
// cache holds a manifest with that model_bundle_id.
std::filesystem::path find_bundle_dir(const std::filesystem::path& cache_root,
                                      const svp::models::ModelLockEntry& entry) {
  const auto holds = [&](const std::filesystem::path& directory) {
    try {
      return svp::models::load_model_bundle_manifest(
                 directory / std::string(kModelManifestFileName))
                 .model_bundle_id == entry.model_bundle_id;
    } catch (const std::exception&) {
      return false;
    }
  };
  if (holds(cache_root / entry.model_id)) {
    return cache_root / entry.model_id;
  }
  std::error_code error;
  for (const auto& candidate : std::filesystem::directory_iterator(cache_root, error)) {
    if (candidate.is_directory() && holds(candidate.path())) {
      return candidate.path();
    }
  }
  throw WorkerError(WorkerErrorCode::configuration,
                    "model bundle " + entry.model_bundle_id + " is not in the model cache " +
                        cache_root.string());
}

}  // namespace

WorkerModelStore::WorkerModelStore(std::filesystem::path models_dir)
    : models_dir_(std::move(models_dir)) {}

std::filesystem::path WorkerModelStore::directory_of(const Blake3Digest& bundle_blake3) const {
  return models_dir_ / blake3_hex(bundle_blake3);
}

bool WorkerModelStore::has(const Blake3Digest& bundle_blake3) const {
  std::error_code error;
  return std::filesystem::is_directory(directory_of(bundle_blake3), error);
}

std::vector<Blake3Digest> WorkerModelStore::list() const {
  std::vector<Blake3Digest> bundles;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(models_dir_, error)) {
    if (const auto digest = parse_blake3_hex(entry.path().filename().string());
        digest && entry.is_directory()) {
      bundles.push_back(*digest);
    }
  }
  std::sort(bundles.begin(), bundles.end());
  return bundles;
}

void WorkerModelStore::install_from_cas(const nlohmann::json& lock, const BlobRef& manifest,
                                        CasStore& cas) const {
  const svp::models::ModelLock parsed = single_entry_lock(lock);
  const svp::models::ModelLockEntry& entry = parsed.models.front();
  const Blake3Digest bundle = bundle_digest(entry);
  if (!safe_relative(entry.model_id) || entry.model_id.find('/') != std::string::npos) {
    throw WorkerError(WorkerErrorCode::protocol, "model_id is not a directory name");
  }
  const std::filesystem::path stage =
      models_dir_ / (".incoming-" + blake3_hex(bundle) + "-" + std::to_string(::getpid()) + "-" +
                     std::to_string(reinterpret_cast<std::uintptr_t>(&parsed)));
  const std::filesystem::path bundle_dir = stage / entry.model_id;
  std::error_code error;
  std::filesystem::remove_all(stage, error);
  std::filesystem::create_directories(bundle_dir, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io, "cannot create " + bundle_dir.string());
  }
  try {
    using std::filesystem::perms;
    constexpr perms kDataMode = perms::owner_read | perms::owner_write;
    copy_cas_blob(cas, manifest, bundle_dir / std::string(kModelManifestFileName), kDataMode);
    for (const svp::models::ModelBundleFile& file : entry.files) {
      const std::optional<Blake3Digest> digest = parse_blake3_hex(file.blake3.hex_value());
      if (!digest || !safe_relative(file.path) || file.path == kModelManifestFileName) {
        throw WorkerError(WorkerErrorCode::protocol,
                          "model lock file entry `" + file.path + "` is not usable");
      }
      // Lock entries carry digests, not sizes; the CAS verifies the bytes.
      CacheResult<std::ifstream> source = cas.open(*digest);
      if (!source) {
        throw WorkerError(WorkerErrorCode::protocol, "model file " + file.path + " (" +
                                                         file.blake3.hex_value() +
                                                         ") was not received");
      }
      const std::filesystem::path target = bundle_dir / file.path;
      std::filesystem::create_directories(target.parent_path(), error);
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      out << source.value().rdbuf();
      out.close();
      if (!out) {
        throw WorkerError(WorkerErrorCode::io, "cannot write " + target.string());
      }
    }
    const svp::models::VerificationReport report =
        svp::models::verify_lock_against_cache(parsed, stage);
    if (!report.ok()) {
      throw WorkerError(WorkerErrorCode::verification,
                        "model bundle " + entry.model_bundle_id + " failed lock verification: " +
                            first_error(report));
    }
    if (has(bundle)) {
      std::filesystem::remove_all(stage, error);
      return;
    }
    std::filesystem::rename(stage, directory_of(bundle), error);
    if (error) {
      std::filesystem::remove_all(stage, error);
      verify(bundle);
    }
  } catch (...) {
    std::filesystem::remove_all(stage, error);
    throw;
  }
}

void WorkerModelStore::verify(const Blake3Digest& bundle_blake3) const {
  const std::filesystem::path root = directory_of(bundle_blake3);
  std::error_code error;
  std::vector<std::filesystem::path> manifests;
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (entry.is_directory()) {
      manifests.push_back(entry.path() / std::string(kModelManifestFileName));
    }
  }
  if (manifests.size() != 1) {
    throw WorkerError(WorkerErrorCode::verification,
                      "model bundle " + blake3_hex(bundle_blake3) + " is not installed");
  }
  const svp::models::VerificationReport report =
      svp::models::verify_extracted_bundle(manifests.front().parent_path());
  if (!report.ok()) {
    throw WorkerError(WorkerErrorCode::verification, first_error(report));
  }
  const svp::models::ModelBundleManifest manifest =
      svp::models::load_model_bundle_manifest(manifests.front());
  if (manifest.bundle_blake3.hex_value() != blake3_hex(bundle_blake3)) {
    throw WorkerError(WorkerErrorCode::verification,
                      "model bundle directory " + root.string() + " holds another bundle");
  }
}

std::vector<ModelBundleSource> prepare_model_bundles(const std::filesystem::path& cache_root,
                                                     const std::vector<std::string>& model_ids) {
  const std::filesystem::path lock_path = cache_root / "model-lock.json";
  std::error_code error;
  if (!std::filesystem::exists(lock_path, error)) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "no model-lock.json in the model cache " + cache_root.string());
  }
  const nlohmann::json lock_json = nlohmann::json::parse(read_text(lock_path));
  const svp::models::ModelLock lock = svp::models::parse_model_lock(lock_json, lock_path.string());
  for (const std::string& wanted : model_ids) {
    const bool known = std::any_of(lock.models.begin(), lock.models.end(),
                                   [&](const auto& entry) { return entry.model_id == wanted; });
    if (!known) {
      throw WorkerError(WorkerErrorCode::configuration,
                        "model " + wanted + " is not in " + lock_path.string());
    }
  }
  std::vector<ModelBundleSource> sources;
  for (std::size_t index = 0; index < lock.models.size(); ++index) {
    const svp::models::ModelLockEntry& entry = lock.models[index];
    if (!model_ids.empty() &&
        std::find(model_ids.begin(), model_ids.end(), entry.model_id) == model_ids.end()) {
      continue;
    }
    const std::filesystem::path bundle_dir = find_bundle_dir(cache_root, entry);
    const svp::models::VerificationReport report =
        svp::models::verify_extracted_bundle(bundle_dir);
    const std::filesystem::path manifest_path = bundle_dir / std::string(kModelManifestFileName);
    if (!report.ok()) {
      throw WorkerError(WorkerErrorCode::verification,
                        "cached model bundle " + bundle_dir.string() + " does not verify: " +
                            first_error(report));
    }
    const svp::models::ModelBundleManifest manifest =
        svp::models::load_model_bundle_manifest(manifest_path);
    if (manifest.model_bundle_id != entry.model_bundle_id ||
        manifest.bundle_blake3.canonical() != entry.bundle_blake3.canonical()) {
      throw WorkerError(WorkerErrorCode::verification,
                        "cached model bundle " + bundle_dir.string() +
                            " is not the bundle model-lock.json pins");
    }
    ModelBundleSource source;
    source.model_id = entry.model_id;
    source.model_bundle_id = entry.model_bundle_id;
    source.bundle_blake3 = bundle_digest(entry);
    source.lock = nlohmann::json{{"model_set_id", lock_json.at("model_set_id")},
                                 {"models", nlohmann::json::array({lock_json.at("models")[index]})},
                                 {"schema_version", lock_json.at("schema_version")}};
    source.manifest = describe_blob_file(manifest_path);
    for (const svp::models::ModelBundleFile& file : entry.files) {
      source.files.push_back(describe_blob_file(bundle_dir / file.path));
    }
    sources.push_back(std::move(source));
  }
  return sources;
}

std::optional<ModelSetSummary> model_set_summary(const std::filesystem::path& cache_root) {
  const std::filesystem::path lock_path = cache_root / "model-lock.json";
  std::error_code error;
  if (!std::filesystem::exists(lock_path, error)) {
    return std::nullopt;
  }
  const std::string bytes = read_text(lock_path);
  const svp::models::ModelLock lock =
      svp::models::parse_model_lock(nlohmann::json::parse(bytes), lock_path.string());
  return ModelSetSummary{.model_set_id = lock.model_set_id, .model_lock_blake3 = blake3_digest(bytes)};
}

}  // namespace svp::exec::worker
