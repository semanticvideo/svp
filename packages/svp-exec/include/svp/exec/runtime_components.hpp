#pragma once

// Reader for the runtime bundle's build record, `components.json`
// (schema `svp.runtime.components/1`, written by
// distribution/runtime-bundle/write-components-manifest.sh). Only the fields
// SVP consumes are read: the target, and each component's shipped files with
// the digest the bundle build recorded. Other fields (licenses, sources,
// build flags) are provenance for people and are left alone.

#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

inline constexpr std::string_view kRuntimeComponentsSchema = "svp.runtime.components/1";

struct RuntimeComponentFile {
  std::string component;
  // Relative to the bundle directory ("bin/ffmpeg").
  std::string path;
  Blake3Digest blake3{};
  std::uint64_t size_bytes = 0;
};

struct RuntimeComponents {
  std::string arch;
  std::string macos_deployment_target;
  std::vector<RuntimeComponentFile> files;

  [[nodiscard]] const RuntimeComponentFile* find(std::string_view path) const noexcept;
};

// Throws ExecError for a malformed record (digests must be "blake3:<hex>") and
// std::system_error when the file cannot be read.
[[nodiscard]] RuntimeComponents load_runtime_components(const std::filesystem::path& path);
[[nodiscard]] RuntimeComponents parse_runtime_components(std::string_view bytes);

}  // namespace svp::exec
