#include "svp/exec/runtime_manifest.hpp"

#include "json_fields.hpp"
#include "record_identifiers.hpp"
#include "runtime_manifest_paths.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace svp::exec {
namespace {

constexpr std::string_view kManifestPath = "runtime_manifest";

void validate_file(const RuntimeManifestFile& file) {
  if (!detail::is_safe_runtime_path(file.path)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "runtime manifest path must be relative and stay inside the "
                    "runtime root: `" + file.path + "`");
  }
  if (!detail::is_record_identifier(file.component)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "runtime manifest component must be [A-Za-z0-9._-]: `" +
                        file.component + "`");
  }
}

void validate_normalized(const RuntimeManifest& manifest) {
  if (manifest.arch.empty() || manifest.macos_deployment_target.empty()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "runtime manifest arch and macos_deployment_target must be set");
  }
  if (manifest.files.empty()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "runtime manifest must list at least one file");
  }
  for (std::size_t index = 0; index < manifest.files.size(); ++index) {
    validate_file(manifest.files[index]);
    if (index > 0 && !(manifest.files[index - 1].path < manifest.files[index].path)) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "runtime manifest files must be sorted by path and unique: `" +
                          manifest.files[index].path + "`");
    }
  }
}

nlohmann::json identity_json(const RuntimeManifest& manifest) {
  validate_normalized(manifest);
  nlohmann::json files = nlohmann::json::array();
  for (const RuntimeManifestFile& file : manifest.files) {
    files.push_back({{"blake3", blake3_hex(file.blake3)},
                     {"component", file.component},
                     {"path", file.path},
                     {"size_bytes", file.size_bytes}});
  }
  return {{"arch", manifest.arch},
          {"files", std::move(files)},
          {"macos_deployment_target", manifest.macos_deployment_target},
          {"schema", std::string(kRuntimeManifestSchema)}};
}

}  // namespace

namespace detail {

bool is_safe_runtime_path(std::string_view path) noexcept {
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = std::min(path.find('/', begin), path.size());
    const std::string_view segment = path.substr(begin, end - begin);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    begin = end + 1;
  }
  return true;
}

}  // namespace detail

void normalize_runtime_manifest(RuntimeManifest& manifest) {
  std::sort(manifest.files.begin(), manifest.files.end(),
            [](const RuntimeManifestFile& left, const RuntimeManifestFile& right) {
              return left.path < right.path;
            });
  validate_normalized(manifest);
}

std::string runtime_manifest_identity_bytes(const RuntimeManifest& manifest) {
  return encode_canonical_json(identity_json(manifest));
}

Blake3Digest compute_runtime_id(const RuntimeManifest& manifest) {
  return blake3_digest(runtime_manifest_identity_bytes(manifest));
}

std::string encode_runtime_manifest(const RuntimeManifest& manifest) {
  nlohmann::json value = identity_json(manifest);
  value["runtime_id"] =
      blake3_prefixed(blake3_digest(encode_canonical_json(value)));
  return encode_canonical_json(value);
}

RuntimeManifest decode_runtime_manifest(std::string_view bytes) {
  const nlohmann::json value = decode_canonical_json(bytes);
  detail::require_object(value, kManifestPath);
  detail::reject_unknown_fields(
      value, {"arch", "files", "macos_deployment_target", "runtime_id", "schema"},
      kManifestPath);

  const std::string schema = detail::required_string(value, "schema", kManifestPath);
  if (schema != kRuntimeManifestSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "unsupported runtime manifest schema `" + schema + "`");
  }

  RuntimeManifest manifest;
  manifest.arch = detail::required_string(value, "arch", kManifestPath);
  manifest.macos_deployment_target =
      detail::required_string(value, "macos_deployment_target", kManifestPath);
  const nlohmann::json& files = detail::required_array(value, "files", kManifestPath);
  const std::string files_path = detail::child_path(kManifestPath, "files");
  for (std::size_t index = 0; index < files.size(); ++index) {
    const std::string path = files_path + "[" + std::to_string(index) + "]";
    const nlohmann::json& entry = files[index];
    detail::require_object(entry, path);
    detail::reject_unknown_fields(entry, {"blake3", "component", "path", "size_bytes"},
                                  path);
    manifest.files.push_back(RuntimeManifestFile{
        .path = detail::required_string(entry, "path", path),
        .component = detail::required_string(entry, "component", path),
        .blake3 = detail::required_blake3_hex(entry, "blake3", path),
        .size_bytes = detail::required_unsigned(entry, "size_bytes", path),
    });
  }
  validate_normalized(manifest);

  const Blake3Digest stored =
      detail::required_blake3_prefixed(value, "runtime_id", kManifestPath);
  const Blake3Digest computed = compute_runtime_id(manifest);
  if (stored != computed) {
    throw ExecError(ExecErrorCode::digest_mismatch,
                    "runtime manifest runtime_id " + blake3_prefixed(stored) +
                        " does not match its content " + blake3_prefixed(computed));
  }
  return manifest;
}

RuntimeManifest load_runtime_manifest(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                            "cannot open runtime manifest " + path.string());
  }
  const std::string bytes{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw std::system_error(std::make_error_code(std::errc::io_error),
                            "cannot read runtime manifest " + path.string());
  }
  return decode_runtime_manifest(bytes);
}

}  // namespace svp::exec
