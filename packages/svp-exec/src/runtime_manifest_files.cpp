#include "svp/exec/runtime_manifest.hpp"

#include "durable_io.hpp"
#include "runtime_manifest_paths.hpp"
#include "svp/exec/exec_error.hpp"

#include <system_error>
#include <utility>

namespace svp::exec {
namespace {

std::filesystem::path resolve_under(const std::filesystem::path& root,
                                    const std::string& relative_path) {
  if (!detail::is_safe_runtime_path(relative_path)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "runtime file path must be relative and stay inside the "
                    "runtime root: `" + relative_path + "`");
  }
  return root / relative_path;
}

}  // namespace

RuntimeManifestFile describe_runtime_file(const std::filesystem::path& root,
                                          std::string relative_path,
                                          std::string component) {
  const std::filesystem::path file = resolve_under(root, relative_path);
  detail::FileDigest digest;
  if (const std::error_code error = detail::hash_file(file, digest)) {
    throw std::system_error(error, "cannot hash runtime file " + file.string());
  }
  return RuntimeManifestFile{.path = std::move(relative_path),
                             .component = std::move(component),
                             .blake3 = digest.digest,
                             .size_bytes = digest.bytes};
}

std::string_view runtime_file_problem_name(RuntimeFileProblem problem) noexcept {
  switch (problem) {
    case RuntimeFileProblem::missing:
      return "missing";
    case RuntimeFileProblem::not_regular_file:
      return "not_regular_file";
    case RuntimeFileProblem::unreadable:
      return "unreadable";
    case RuntimeFileProblem::size_mismatch:
      return "size_mismatch";
    case RuntimeFileProblem::digest_mismatch:
      return "digest_mismatch";
  }
  return "unknown";
}

std::vector<RuntimeFileFinding> verify_runtime_files(
    const RuntimeManifest& manifest, const std::filesystem::path& root) {
  std::vector<RuntimeFileFinding> findings;
  for (const RuntimeManifestFile& expected : manifest.files) {
    const std::filesystem::path file = resolve_under(root, expected.path);
    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::status(file, status_error);
    if (!std::filesystem::exists(status)) {
      findings.push_back({expected.path, RuntimeFileProblem::missing, file.string()});
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) {
      findings.push_back(
          {expected.path, RuntimeFileProblem::not_regular_file, file.string()});
      continue;
    }
    // Size first: a cheap check that catches truncation without hashing.
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(file, size_error);
    if (size_error) {
      findings.push_back(
          {expected.path, RuntimeFileProblem::unreadable, size_error.message()});
      continue;
    }
    if (size != expected.size_bytes) {
      findings.push_back({expected.path, RuntimeFileProblem::size_mismatch,
                          "expected " + std::to_string(expected.size_bytes) +
                              " bytes, found " + std::to_string(size)});
      continue;
    }
    detail::FileDigest digest;
    if (const std::error_code error = detail::hash_file(file, digest)) {
      findings.push_back(
          {expected.path, RuntimeFileProblem::unreadable, error.message()});
      continue;
    }
    if (digest.digest != expected.blake3 || digest.bytes != expected.size_bytes) {
      findings.push_back({expected.path, RuntimeFileProblem::digest_mismatch,
                          "expected " + blake3_hex(expected.blake3) + ", found " +
                              blake3_hex(digest.digest)});
    }
  }
  return findings;
}

}  // namespace svp::exec
