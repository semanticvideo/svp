#include "svp/exec/worker/runtime_store.hpp"

#include "runtime_files.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <algorithm>
#include <fstream>
#include <unistd.h>

namespace svp::exec::worker {
namespace {

std::string blob_name(const BlobRef& blob) { return "blob " + blake3_hex(blob.blake3); }

}  // namespace

std::vector<std::byte> read_cas_blob(CasStore& cas, const BlobRef& blob) {
  CacheResult<std::vector<std::byte>> bytes = cas.get(blob.blake3);
  if (!bytes) {
    throw WorkerError(bytes.error().code == CacheErrorCode::corrupt ? WorkerErrorCode::verification
                                                                    : WorkerErrorCode::protocol,
                      blob_name(blob) + " is not available: " + bytes.error().message);
  }
  if (bytes.value().size() != blob.bytes) {
    throw WorkerError(WorkerErrorCode::verification, blob_name(blob) + " has the wrong length");
  }
  return std::move(bytes).value();
}

void copy_cas_blob(CasStore& cas, const BlobRef& blob, const std::filesystem::path& target,
                   std::filesystem::perms mode) {
  CacheResult<std::ifstream> source = cas.open(blob.blake3);
  if (!source) {
    throw WorkerError(source.error().code == CacheErrorCode::corrupt
                          ? WorkerErrorCode::verification
                          : WorkerErrorCode::protocol,
                      blob_name(blob) + " is not available: " + source.error().message);
  }
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  out << source.value().rdbuf();
  out.close();
  if (!out) {
    throw WorkerError(WorkerErrorCode::io, "cannot write " + target.string());
  }
  if (std::filesystem::file_size(target, error) != blob.bytes || error) {
    throw WorkerError(WorkerErrorCode::verification,
                      blob_name(blob) + " copied to " + target.string() + " has the wrong length");
  }
  std::filesystem::permissions(target, mode, std::filesystem::perm_options::replace, error);
}

Blake3Digest verify_runtime_directory(const std::filesystem::path& runtime_dir,
                                      const std::optional<Blake3Digest>& expected) {
  RuntimeManifest manifest;
  try {
    manifest = load_runtime_manifest(runtime_manifest_path(runtime_dir));
  } catch (const std::system_error& error) {
    throw WorkerError(WorkerErrorCode::io, "cannot read the manifest of runtime " +
                                               runtime_dir.string() + ": " + error.what());
  } catch (const std::exception& error) {
    throw WorkerError(WorkerErrorCode::verification,
                      "runtime " + runtime_dir.string() + " has an invalid manifest: " +
                          error.what());
  }
  const Blake3Digest runtime_id = compute_runtime_id(manifest);
  if (expected && *expected != runtime_id) {
    throw WorkerError(WorkerErrorCode::verification,
                      "runtime " + runtime_dir.string() + " is " + blake3_prefixed(runtime_id) +
                          ", expected " + blake3_prefixed(*expected));
  }
  bool has_program = false;
  for (const RuntimeManifestFile& file : manifest.files) {
    has_program = has_program || file.path == kSessionProgram;
  }
  if (!has_program) {
    throw WorkerError(WorkerErrorCode::verification,
                      "runtime " + blake3_prefixed(runtime_id) + " does not contain " +
                          std::string(kSessionProgram));
  }
  const std::vector<RuntimeFileFinding> findings = verify_runtime_files(manifest, runtime_dir);
  if (!findings.empty()) {
    const RuntimeFileFinding& first = findings.front();
    throw WorkerError(WorkerErrorCode::verification,
                      "runtime " + blake3_prefixed(runtime_id) + ": " + first.path + " " +
                          std::string(runtime_file_problem_name(first.problem)) +
                          (first.detail.empty() ? "" : " (" + first.detail + ")"));
  }
  return runtime_id;
}

WorkerRuntimeStore::WorkerRuntimeStore(std::filesystem::path runtimes_dir)
    : runtimes_dir_(std::move(runtimes_dir)) {}

std::filesystem::path WorkerRuntimeStore::directory_of(const Blake3Digest& runtime_id) const {
  return runtimes_dir_ / blake3_hex(runtime_id);
}

bool WorkerRuntimeStore::has(const Blake3Digest& runtime_id) const {
  try {
    return compute_runtime_id(load_runtime_manifest(
               runtime_manifest_path(directory_of(runtime_id)))) == runtime_id;
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<Blake3Digest> WorkerRuntimeStore::list() const {
  std::vector<Blake3Digest> runtimes;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(runtimes_dir_, error)) {
    const std::optional<Blake3Digest> id = parse_blake3_hex(entry.path().filename().string());
    if (id && has(*id)) {
      runtimes.push_back(*id);
    }
  }
  std::sort(runtimes.begin(), runtimes.end());
  return runtimes;
}

void WorkerRuntimeStore::verify(const Blake3Digest& runtime_id) const {
  (void)verify_runtime_directory(directory_of(runtime_id), runtime_id);
}

void WorkerRuntimeStore::install_from_cas(const Blake3Digest& runtime_id, const BlobRef& manifest,
                                          const std::optional<BlobRef>& components,
                                          CasStore& cas) const {
  const std::vector<std::byte> manifest_bytes = read_cas_blob(cas, manifest);
  RuntimeManifest decoded;
  try {
    decoded = decode_runtime_manifest(std::string_view(
        reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()));
  } catch (const std::exception& error) {
    throw WorkerError(WorkerErrorCode::verification,
                      std::string("RUNTIME_PUT manifest is invalid: ") + error.what());
  }
  if (compute_runtime_id(decoded) != runtime_id) {
    throw WorkerError(WorkerErrorCode::verification,
                      "RUNTIME_PUT manifest does not hash to " + blake3_prefixed(runtime_id));
  }
  const std::filesystem::path target = directory_of(runtime_id);
  const std::filesystem::path stage =
      runtimes_dir_ / (".incoming-" + blake3_hex(runtime_id) + "-" + std::to_string(::getpid()) +
                       "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&decoded)));
  std::error_code error;
  std::filesystem::remove_all(stage, error);
  std::filesystem::create_directories(stage, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io, "cannot create " + stage.string());
  }
  try {
    for (const RuntimeManifestFile& file : decoded.files) {
      copy_cas_blob(cas, BlobRef{.blake3 = file.blake3, .bytes = file.size_bytes},
                    stage / file.path, detail::runtime_file_mode(file.path));
    }
    detail::write_text_file(runtime_manifest_path(stage),
                            std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()),
                                             manifest_bytes.size()));
    if (components) {
      const std::vector<std::byte> bytes = read_cas_blob(cas, *components);
      detail::write_text_file(runtime_components_path(stage),
                              std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size()));
    }
    (void)verify_runtime_directory(stage, runtime_id);
    bool existing_verifies = false;
    if (has(runtime_id)) {
      try {
        verify(runtime_id);
        existing_verifies = true;
      } catch (const WorkerError&) {
        // A damaged copy is replaced by the verified one below.
      }
    }
    if (existing_verifies) {
      std::filesystem::remove_all(stage, error);
      return;
    }
    std::filesystem::remove_all(target, error);
    std::filesystem::rename(stage, target, error);
    if (error) {
      // Another session installed it first; keep theirs when it verifies.
      std::filesystem::remove_all(stage, error);
      verify(runtime_id);
    }
  } catch (...) {
    std::filesystem::remove_all(stage, error);
    throw;
  }
}

}  // namespace svp::exec::worker
