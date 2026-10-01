#pragma once

// Runtime identity (plan §3.2, §4.1): the executables and libraries a build
// runs, each with its BLAKE3 digest, and a `runtime_id` over all of them. A
// coordinator and a worker agree on a runtime by comparing runtime_ids, and a
// worker verifies every file of a runtime before it spawns it.
//
// On-disk form (`manifest.json`, written as canonical JSON, see
// canonical_json.hpp):
//
//   {"arch":"arm64",
//    "files":[{"blake3":"<64 hex>","component":"svp-builder",
//              "path":"bin/svp-builder","size_bytes":123}, ...],
//    "macos_deployment_target":"15.0",
//    "runtime_id":"b3:<64 hex>",
//    "schema":"svp.runtime.manifest/1"}
//
// `runtime_id` is BLAKE3 over the canonical JSON of the same object without
// the `runtime_id` member. Files are sorted by `path` (unsigned byte order)
// and unique, so one set of files has exactly one manifest and one
// runtime_id. `path` is relative to the runtime root (the install prefix),
// '/'-separated, and never absolute or escaping the root.

#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

inline constexpr std::string_view kRuntimeManifestSchema = "svp.runtime.manifest/1";

struct RuntimeManifestFile {
  std::string path;
  // Which build produced the file ("svp-builder", "ffmpeg", "sherpa-onnx").
  std::string component;
  Blake3Digest blake3{};
  std::uint64_t size_bytes = 0;

  friend bool operator==(const RuntimeManifestFile&,
                         const RuntimeManifestFile&) = default;
};

struct RuntimeManifest {
  std::string arch;
  std::string macos_deployment_target;
  std::vector<RuntimeManifestFile> files;

  friend bool operator==(const RuntimeManifest&, const RuntimeManifest&) = default;
};

// Sorts `files` by path. Throws ExecError(invalid_value) for a duplicate or
// unsafe path, an invalid component name, or empty arch/target fields.
void normalize_runtime_manifest(RuntimeManifest& manifest);

// Canonical JSON of the manifest without `runtime_id`: the runtime_id input.
// Requires a normalized manifest (throws ExecError(invalid_value) otherwise).
[[nodiscard]] std::string runtime_manifest_identity_bytes(const RuntimeManifest& manifest);

[[nodiscard]] Blake3Digest compute_runtime_id(const RuntimeManifest& manifest);

// The on-disk bytes: canonical JSON including `runtime_id`.
[[nodiscard]] std::string encode_runtime_manifest(const RuntimeManifest& manifest);

// Parses the on-disk bytes. Requires canonical JSON, the known fields only,
// normalized files, and a stored runtime_id equal to compute_runtime_id().
// Throws ExecError (non_canonical_json, missing_field, unknown_field,
// wrong_type, invalid_value, invalid_digest, or digest_mismatch).
[[nodiscard]] RuntimeManifest decode_runtime_manifest(std::string_view bytes);

// Reads and decodes a manifest file. Throws std::system_error when the file
// cannot be read, otherwise as decode_runtime_manifest().
[[nodiscard]] RuntimeManifest load_runtime_manifest(const std::filesystem::path& path);

// --- Files on disk ------------------------------------------------------------

// Hashes `root / relative_path`. Throws std::system_error when it cannot be
// read and ExecError(invalid_value) for an unsafe relative path.
[[nodiscard]] RuntimeManifestFile describe_runtime_file(
    const std::filesystem::path& root, std::string relative_path,
    std::string component);

enum class RuntimeFileProblem {
  missing,
  not_regular_file,
  unreadable,
  size_mismatch,
  digest_mismatch,
};

[[nodiscard]] std::string_view runtime_file_problem_name(RuntimeFileProblem problem) noexcept;

struct RuntimeFileFinding {
  std::string path;
  RuntimeFileProblem problem = RuntimeFileProblem::missing;
  std::string detail;
};

// Checks every manifest file under `root` (size, then full BLAKE3). An empty
// result means the runtime on disk is exactly the one the manifest names.
[[nodiscard]] std::vector<RuntimeFileFinding> verify_runtime_files(
    const RuntimeManifest& manifest, const std::filesystem::path& root);

}  // namespace svp::exec
