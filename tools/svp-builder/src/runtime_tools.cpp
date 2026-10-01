#include "svp/builder/runtime_tools.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/runtime_components.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/runtime_manifest_assembly.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace svp::builder {
namespace {

// Bundle paths mirror the bundle build's output layout
// (distribution/runtime-bundle/README.md, "Output layout").
constexpr std::array<RuntimeToolSpec, 3> kRuntimeToolSpecs{{
    {RuntimeTool::ffmpeg, "ffmpeg", "SVP_FFMPEG", "bin/ffmpeg"},
    {RuntimeTool::ffprobe, "ffprobe", "SVP_FFPROBE", "bin/ffprobe"},
    {RuntimeTool::sherpa_onnx, "sherpa-onnx", "SHERPA_ONNX_LIB_PATH",
     "lib/libsherpa-onnx-c-api.dylib"},
}};

std::runtime_error bundle_error(const std::filesystem::path& root,
                                const std::string& detail) {
  return std::runtime_error(
      "the SVP runtime bundle installed at " + root.string() +
      " is unusable: " + detail +
      ". Reinstall it, or remove the directory to use tools from PATH.");
}

}  // namespace

const RuntimeToolSpec& runtime_tool_spec(RuntimeTool tool) {
  for (const RuntimeToolSpec& spec : kRuntimeToolSpecs) {
    if (spec.tool == tool) return spec;
  }
  throw std::logic_error("unknown runtime tool");
}

std::string_view runtime_tool_source_name(RuntimeToolSource source) {
  switch (source) {
    case RuntimeToolSource::explicit_flag:
      return "explicit_flag";
    case RuntimeToolSource::environment:
      return "environment";
    case RuntimeToolSource::bundled:
      return "bundled";
    case RuntimeToolSource::path_search:
      return "path_search";
  }
  return "path_search";
}

std::optional<RuntimeBundle> locate_runtime_bundle(
    const std::filesystem::path& executable) {
  if (executable.empty()) return std::nullopt;
  const std::filesystem::path root =
      (executable.parent_path() / std::string(kRuntimeBundleDirFromExecutableDir))
          .lexically_normal();
  const std::filesystem::path components_path =
      root / std::string(svp::exec::kRuntimeComponentsFileName);
  std::error_code error;
  if (!std::filesystem::exists(components_path, error)) {
    return std::nullopt;
  }

  RuntimeBundle bundle;
  bundle.root = root;
  try {
    const svp::exec::RuntimeComponents components =
        svp::exec::load_runtime_components(components_path);
    for (const svp::exec::RuntimeComponentFile& file : components.files) {
      bundle.file_blake3[file.path] = "blake3:" + svp::exec::blake3_hex(file.blake3);
    }
    const std::filesystem::path manifest_path =
        root / std::string(svp::exec::kRuntimeManifestFileName);
    if (std::filesystem::exists(manifest_path, error)) {
      // Decoding checks that runtime_id matches the manifest content; the
      // files themselves are verified by the worker, not on every build.
      bundle.runtime_id = svp::exec::blake3_prefixed(svp::exec::compute_runtime_id(
          svp::exec::load_runtime_manifest(manifest_path)));
    }
  } catch (const std::exception& failure) {
    throw bundle_error(root, failure.what());
  }
  return bundle;
}

EnvironmentLookup process_environment() {
  return [](std::string_view name) -> std::optional<std::string> {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || value[0] == '\0') return std::nullopt;
    return std::string(value);
  };
}

std::optional<RuntimeToolChoice> bundled_runtime_tool(
    RuntimeTool tool, const std::optional<RuntimeBundle>& bundle) {
  if (!bundle) return std::nullopt;
  const RuntimeToolSpec& spec = runtime_tool_spec(tool);
  const auto declared = bundle->file_blake3.find(std::string(spec.bundle_path));
  if (declared == bundle->file_blake3.end()) return std::nullopt;

  const std::filesystem::path path = bundle->root / std::string(spec.bundle_path);
  std::error_code error;
  const bool must_execute = tool != RuntimeTool::sherpa_onnx;
  if (!std::filesystem::is_regular_file(path, error) ||
      (must_execute && ::access(path.c_str(), X_OK) != 0)) {
    throw bundle_error(bundle->root, "components.json lists " +
                                         std::string(spec.bundle_path) +
                                         " but it is missing or not usable");
  }
  return RuntimeToolChoice{.path = path.string(),
                           .source = RuntimeToolSource::bundled,
                           .blake3 = declared->second};
}

RuntimeToolChoice resolve_runtime_tool(RuntimeTool tool,
                                       const std::optional<std::string>& flag_value,
                                       const std::optional<RuntimeBundle>& bundle,
                                       const EnvironmentLookup& environment) {
  if (tool == RuntimeTool::sherpa_onnx) {
    throw std::logic_error("sherpa-onnx search order is owned by svp-audio");
  }
  if (flag_value) {
    return {.path = *flag_value, .source = RuntimeToolSource::explicit_flag};
  }
  const RuntimeToolSpec& spec = runtime_tool_spec(tool);
  if (const std::optional<std::string> value = environment(spec.environment_variable)) {
    return {.path = *value, .source = RuntimeToolSource::environment};
  }
  if (std::optional<RuntimeToolChoice> bundled = bundled_runtime_tool(tool, bundle)) {
    return *std::move(bundled);
  }
  return {.path = std::string(spec.name), .source = RuntimeToolSource::path_search};
}

}  // namespace svp::builder
