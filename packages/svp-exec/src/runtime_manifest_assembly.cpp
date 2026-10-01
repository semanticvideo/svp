#include "svp/exec/runtime_manifest_assembly.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/runtime_components.hpp"

namespace svp::exec {

RuntimeManifest assemble_runtime_manifest(
    const std::filesystem::path& root, std::string_view bundle_dir,
    const std::vector<RuntimeManifestExtraFile>& extra_files) {
  const std::string prefix = std::string(bundle_dir) + "/";
  const RuntimeComponents components = load_runtime_components(
      root / std::string(bundle_dir) / std::string(kRuntimeComponentsFileName));

  RuntimeManifest manifest;
  manifest.arch = components.arch;
  manifest.macos_deployment_target = components.macos_deployment_target;
  for (const RuntimeComponentFile& recorded : components.files) {
    RuntimeManifestFile file =
        describe_runtime_file(root, prefix + recorded.path, recorded.component);
    if (file.blake3 != recorded.blake3 || file.size_bytes != recorded.size_bytes) {
      throw ExecError(ExecErrorCode::digest_mismatch,
                      "installed runtime file " + file.path +
                          " differs from its components.json record (expected " +
                          blake3_hex(recorded.blake3) + ", found " +
                          blake3_hex(file.blake3) + ")");
    }
    manifest.files.push_back(std::move(file));
  }
  for (const RuntimeManifestExtraFile& extra : extra_files) {
    manifest.files.push_back(describe_runtime_file(root, extra.path, extra.component));
  }
  normalize_runtime_manifest(manifest);
  return manifest;
}

}  // namespace svp::exec
