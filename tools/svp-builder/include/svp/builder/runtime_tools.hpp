#pragma once

// Where svp-builder finds the external programs and libraries it runs:
// ffmpeg, ffprobe, and the sherpa-onnx C API library.
//
// Resolution order for each tool (first match wins):
//   1. the command-line flag (--ffmpeg, --ffprobe, --sherpa-lib);
//   2. an environment override (SVP_FFMPEG, SVP_FFPROBE, SHERPA_ONNX_LIB_PATH);
//   3. the runtime bundle installed next to the real svp-builder executable
//      (<exe dir>/../libexec/svp/runtime, see distribution/runtime-bundle);
//   4. the previous defaults: `ffmpeg`/`ffprobe` looked up on PATH, and the
//      legacy sherpa-onnx search (pip, venv/Conda, Homebrew, system paths).
//
// Without a bundle, steps 1, 2 (for sherpa), and 4 are exactly the behaviour
// before bundles existed. Which source each tool came from is recorded in the
// builder foundation JSON and the run report, never in the package.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder {

// The installed layout (cmake/SvpInstall.cmake): the bundle sits in
// <prefix>/libexec/svp/runtime and svp-builder in <prefix>/bin.
inline constexpr std::string_view kRuntimeBundleDirFromExecutableDir =
    "../libexec/svp/runtime";

enum class RuntimeTool { ffmpeg, ffprobe, sherpa_onnx };

struct RuntimeToolSpec {
  RuntimeTool tool;
  // Tool name; for ffmpeg/ffprobe also the PATH-search default.
  std::string_view name;
  // Override variable, checked after the flag and before the bundle.
  std::string_view environment_variable;
  // Location inside the bundle, as recorded in its components.json.
  std::string_view bundle_path;
};

[[nodiscard]] const RuntimeToolSpec& runtime_tool_spec(RuntimeTool tool);

enum class RuntimeToolSource { explicit_flag, environment, bundled, path_search };

[[nodiscard]] std::string_view runtime_tool_source_name(RuntimeToolSource source);

struct RuntimeBundle {
  std::filesystem::path root;
  // Bundle-relative path -> digest exactly as components.json records it
  // ("blake3:<hex>").
  std::map<std::string, std::string> file_blake3;
  // "b3:<hex>" from the bundle's manifest.json; empty when there is none.
  std::string runtime_id;
};

// The bundle next to `executable`, which must already be the real
// (symlink-resolved) executable path. nullopt when no bundle is installed
// there (no components.json). Throws std::runtime_error when a bundle is
// installed but its components.json or manifest.json cannot be read or is
// invalid, rather than silently running other tools.
[[nodiscard]] std::optional<RuntimeBundle> locate_runtime_bundle(
    const std::filesystem::path& executable);

struct RuntimeToolChoice {
  // What the stages receive: an absolute path, or a bare name for PATH search.
  std::string path;
  RuntimeToolSource source = RuntimeToolSource::path_search;
  // Bundled tools only: the digest components.json records.
  std::string blake3;
};

using EnvironmentLookup =
    std::function<std::optional<std::string>(std::string_view name)>;

// getenv(); empty values count as unset.
[[nodiscard]] EnvironmentLookup process_environment();

// ffmpeg or ffprobe. `flag_value` is set when the user passed the tool's flag.
// Throws std::runtime_error when the bundle declares the tool but the file is
// not an executable regular file.
[[nodiscard]] RuntimeToolChoice resolve_runtime_tool(
    RuntimeTool tool, const std::optional<std::string>& flag_value,
    const std::optional<RuntimeBundle>& bundle, const EnvironmentLookup& environment);

// The bundle's copy of `tool`, if the bundle declares one. Used for the
// sherpa-onnx library, whose remaining search order svp-audio owns.
[[nodiscard]] std::optional<RuntimeToolChoice> bundled_runtime_tool(
    RuntimeTool tool, const std::optional<RuntimeBundle>& bundle);

// What one command resolved. Tools the command does not use stay empty.
struct RuntimeToolSelection {
  std::optional<std::filesystem::path> bundle_root;
  std::string runtime_id;
  std::optional<RuntimeToolChoice> ffmpeg;
  std::optional<RuntimeToolChoice> ffprobe;
  // True when the command may load sherpa-onnx; which library actually
  // loaded is read from svp-audio when the selection is reported.
  bool uses_sherpa = false;
  std::optional<RuntimeToolChoice> sherpa_bundled;
};

}  // namespace svp::builder
