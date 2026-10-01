#pragma once

#include "svp/exec/runtime_manifest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec {

// The name of the runtime identity file inside an installed bundle directory.
inline constexpr std::string_view kRuntimeManifestFileName = "manifest.json";
// The bundle build record inside the same directory.
inline constexpr std::string_view kRuntimeComponentsFileName = "components.json";

struct RuntimeManifestExtraFile {
  std::string component;
  // Relative to the runtime root.
  std::string path;
};

// Builds the manifest of an installed runtime rooted at `root` (the install
// prefix). `bundle_dir` is the bundle directory relative to `root`
// ("libexec/svp/runtime"); every file its components.json lists is included
// under that directory, hashed, and checked against the digest and size the
// bundle build recorded. `extra_files` adds files that are not part of the
// bundle build, such as `bin/svp-builder`.
//
// Throws ExecError(digest_mismatch) when a bundled file differs from its
// components.json record, ExecError(invalid_value) for unsafe or duplicate
// paths, and std::system_error when a file cannot be read.
[[nodiscard]] RuntimeManifest assemble_runtime_manifest(
    const std::filesystem::path& root, std::string_view bundle_dir,
    const std::vector<RuntimeManifestExtraFile>& extra_files);

}  // namespace svp::exec
