#include "runtime_tools_json.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/builder/runtime_tools.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/runtime_manifest_assembly.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using svp::builder::RuntimeTool;
using svp::builder::RuntimeToolSource;

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void expect_equal(const std::string& actual, const std::string& expected,
                  const std::string& message) {
  if (actual != expected) {
    throw std::runtime_error(message + "\n  expected: " + expected +
                             "\n  actual:   " + actual);
  }
}

template <typename Function>
void expect_runtime_error(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const std::runtime_error&) {
    return;
  }
  throw std::runtime_error(message + ": no runtime_error was thrown");
}

void write_file(const fs::path& path, const std::string& text, bool executable = false) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  output.close();
  if (executable) {
    fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
  }
}

// An install prefix with bin/svp-builder and, optionally, a runtime bundle
// laid out as cmake --install places it.
struct InstallPrefix {
  fs::path root;

  InstallPrefix() {
    static std::atomic<int> counter{0};
    root = fs::temp_directory_path() /
           ("svp-runtime-tools-" + std::to_string(::getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(counter.fetch_add(1)));
    write_file(executable(), "builder", true);
  }
  ~InstallPrefix() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }

  [[nodiscard]] fs::path executable() const { return root / "bin/svp-builder"; }
  [[nodiscard]] fs::path bundle() const { return root / "libexec/svp/runtime"; }

  // components.json declaring `files` (bundle path -> content); each file is
  // written too unless listed in `missing`.
  void install_bundle(const std::map<std::string, std::string>& files,
                      const std::string& missing = {}) const {
    nlohmann::json component_files = nlohmann::json::array();
    for (const auto& [path, content] : files) {
      if (path != missing) {
        write_file(bundle() / path, content, path.rfind("bin/", 0) == 0);
      }
      // The digest value only has to be well formed here.
      component_files.push_back({{"path", path},
                                 {"blake3", "blake3:" + std::string(64, 'a')},
                                 {"size_bytes", content.size()}});
    }
    const nlohmann::json components = {
        {"schema", "svp.runtime.components/1"},
        {"arch", "arm64"},
        {"macos_deployment_target", "15.0"},
        {"components", {{{"component", "test"}, {"files", component_files}}}},
        {"support_files", nlohmann::json::array()},
    };
    write_file(bundle() / "components.json", components.dump(2));
  }
};

svp::builder::EnvironmentLookup environment(std::map<std::string, std::string> values) {
  return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
    const auto found = values.find(std::string(name));
    if (found == values.end()) return std::nullopt;
    return found->second;
  };
}

const std::map<std::string, std::string> kFullBundle = {
    {"bin/ffmpeg", "ffmpeg"},
    {"bin/ffprobe", "ffprobe"},
    {"lib/libsherpa-onnx-c-api.dylib", "sherpa"},
    {"lib/libonnxruntime.1.dylib", "ort"},
};

void test_no_bundle_keeps_path_defaults() {
  const InstallPrefix prefix;
  const auto bundle = svp::builder::locate_runtime_bundle(prefix.executable());
  expect(!bundle.has_value(), "no components.json means no bundle");
  for (const RuntimeTool tool : {RuntimeTool::ffmpeg, RuntimeTool::ffprobe}) {
    const auto choice =
        svp::builder::resolve_runtime_tool(tool, std::nullopt, bundle, environment({}));
    expect_equal(choice.path, std::string(svp::builder::runtime_tool_spec(tool).name),
                 "without a bundle the tool is the bare name looked up on PATH");
    expect(choice.source == RuntimeToolSource::path_search, "path_search source");
    expect(choice.blake3.empty(), "no digest for PATH tools");
  }
  expect(!svp::builder::bundled_runtime_tool(RuntimeTool::sherpa_onnx, bundle),
         "no bundled sherpa without a bundle");
}

void test_bundle_is_used_after_flag_and_environment() {
  const InstallPrefix prefix;
  prefix.install_bundle(kFullBundle);
  const auto bundle = svp::builder::locate_runtime_bundle(prefix.executable());
  expect(bundle.has_value(), "bundle found next to the executable");
  expect(fs::equivalent(bundle->root, prefix.bundle()), "bundle root");
  expect(bundle->runtime_id.empty(), "no manifest.json, no runtime_id");

  const auto bundled = svp::builder::resolve_runtime_tool(
      RuntimeTool::ffmpeg, std::nullopt, bundle, environment({}));
  expect(bundled.source == RuntimeToolSource::bundled, "bundle beats PATH");
  expect(fs::equivalent(bundled.path, prefix.bundle() / "bin/ffmpeg"), "bundled path");
  expect_equal(bundled.blake3, "blake3:" + std::string(64, 'a'),
               "digest recorded from components.json");

  const auto from_environment = svp::builder::resolve_runtime_tool(
      RuntimeTool::ffmpeg, std::nullopt, bundle, environment({{"SVP_FFMPEG", "/env/ffmpeg"}}));
  expect(from_environment.source == RuntimeToolSource::environment, "env beats bundle");
  expect_equal(from_environment.path, "/env/ffmpeg", "env path");

  const auto from_flag = svp::builder::resolve_runtime_tool(
      RuntimeTool::ffprobe, std::string("/flag/ffprobe"), bundle,
      environment({{"SVP_FFPROBE", "/env/ffprobe"}}));
  expect(from_flag.source == RuntimeToolSource::explicit_flag, "flag beats env");
  expect_equal(from_flag.path, "/flag/ffprobe", "flag path");

  const auto sherpa = svp::builder::bundled_runtime_tool(RuntimeTool::sherpa_onnx, bundle);
  expect(sherpa.has_value(), "bundled sherpa offered");
  expect(fs::equivalent(sherpa->path, prefix.bundle() / "lib/libsherpa-onnx-c-api.dylib"),
         "bundled sherpa path");
}

void test_bundle_without_a_tool_falls_back_to_path() {
  const InstallPrefix prefix;
  prefix.install_bundle({{"lib/libsherpa-onnx-c-api.dylib", "sherpa"}});
  const auto bundle = svp::builder::locate_runtime_bundle(prefix.executable());
  const auto choice = svp::builder::resolve_runtime_tool(
      RuntimeTool::ffmpeg, std::nullopt, bundle, environment({}));
  expect(choice.source == RuntimeToolSource::path_search,
         "a tool the bundle does not declare comes from PATH");
}

// SHERPA_ONNX_LIB_PATH for one scope, restored after.
class ScopedSherpaEnvironment {
 public:
  explicit ScopedSherpaEnvironment(const std::optional<std::string>& value) {
    if (const char* previous = std::getenv(kVariable)) previous_ = previous;
    if (value) {
      ::setenv(kVariable, value->c_str(), 1);
    } else {
      ::unsetenv(kVariable);
    }
  }
  ~ScopedSherpaEnvironment() {
    if (previous_) {
      ::setenv(kVariable, previous_->c_str(), 1);
    } else {
      ::unsetenv(kVariable);
    }
  }

 private:
  static constexpr const char* kVariable = "SHERPA_ONNX_LIB_PATH";
  std::optional<std::string> previous_;
};

// Every command that names sherpa-onnx (a build, a worker session, `workers
// sync`) offers the bundle's library the same way, so svp-audio's search
// finds the library a build of this runtime loads: SHERPA_ONNX_LIB_PATH
// first, then the bundle's pinned copy.
void test_bundled_sherpa_library_is_offered_to_the_search() {
  const InstallPrefix prefix;
  prefix.install_bundle(kFullBundle);
  const auto bundle = svp::builder::locate_runtime_bundle(prefix.executable());
  const fs::path bundled = prefix.bundle() / "lib/libsherpa-onnx-c-api.dylib";
  {
    const ScopedSherpaEnvironment unset(std::nullopt);
    const auto offered = svp::builder::offer_bundled_sherpa_library(bundle);
    expect(offered && fs::equivalent(offered->path, bundled), "the bundle's library is offered");
    expect(offered->source == RuntimeToolSource::bundled, "offered as bundled");
    const std::string expected = svp::audio::sherpa_lib_path_expected();
    expect(!expected.empty() && fs::equivalent(expected, bundled),
           "the search finds the bundle's library: " + expected);
  }
  {
    const fs::path override_library = prefix.root / "override/libsherpa-onnx-c-api.dylib";
    write_file(override_library, "override");
    const ScopedSherpaEnvironment set(override_library.string());
    expect_equal(svp::audio::sherpa_lib_path_expected(), override_library.string(),
                 "SHERPA_ONNX_LIB_PATH precedes the bundle");
  }
  svp::audio::set_sherpa_bundled_lib_path({});

  const InstallPrefix bare;
  bare.install_bundle({{"bin/ffmpeg", "ffmpeg"}});
  const auto no_library = svp::builder::locate_runtime_bundle(bare.executable());
  expect(!svp::builder::offer_bundled_sherpa_library(no_library) &&
             !svp::builder::offer_bundled_sherpa_library(std::nullopt),
         "nothing is offered without a bundled library");
}

void test_broken_bundle_is_an_error() {
  {
    const InstallPrefix prefix;
    prefix.install_bundle(kFullBundle, "bin/ffmpeg");
    const auto bundle = svp::builder::locate_runtime_bundle(prefix.executable());
    expect_runtime_error(
        [&] {
          static_cast<void>(svp::builder::resolve_runtime_tool(
              RuntimeTool::ffmpeg, std::nullopt, bundle, environment({})));
        },
        "declared but missing tool");
  }
  {
    const InstallPrefix prefix;
    write_file(prefix.bundle() / "components.json", "{not json");
    expect_runtime_error(
        [&] { static_cast<void>(svp::builder::locate_runtime_bundle(prefix.executable())); },
        "unreadable components.json");
  }
  {
    const InstallPrefix prefix;
    prefix.install_bundle(kFullBundle);
    write_file(prefix.bundle() / "manifest.json", "{}");
    expect_runtime_error(
        [&] { static_cast<void>(svp::builder::locate_runtime_bundle(prefix.executable())); },
        "invalid manifest.json");
  }
}

void test_runtime_id_from_manifest() {
  const InstallPrefix prefix;
  prefix.install_bundle({{"bin/ffmpeg", "ffmpeg"}});
  svp::exec::RuntimeManifest manifest;
  manifest.arch = "arm64";
  manifest.macos_deployment_target = "15.0";
  manifest.files = {
      svp::exec::describe_runtime_file(prefix.root, "bin/svp-builder", "svp-builder"),
      svp::exec::describe_runtime_file(prefix.root, "libexec/svp/runtime/bin/ffmpeg",
                                       "ffmpeg")};
  svp::exec::normalize_runtime_manifest(manifest);
  write_file(prefix.bundle() / std::string(svp::exec::kRuntimeManifestFileName),
             svp::exec::encode_runtime_manifest(manifest));

  const auto bundle = svp::builder::locate_runtime_bundle(prefix.executable());
  expect_equal(bundle->runtime_id,
               svp::exec::blake3_prefixed(svp::exec::compute_runtime_id(manifest)),
               "runtime_id read from manifest.json");
}

void test_selection_json() {
  svp::builder::RuntimeToolSelection selection;
  selection.bundle_root = "/prefix/libexec/svp/runtime";
  selection.runtime_id = "b3:" + std::string(64, 'b');
  selection.ffmpeg = svp::builder::RuntimeToolChoice{
      .path = "/prefix/libexec/svp/runtime/bin/ffmpeg",
      .source = RuntimeToolSource::bundled,
      .blake3 = "blake3:" + std::string(64, 'a')};
  selection.ffprobe = svp::builder::RuntimeToolChoice{.path = "ffprobe"};
  selection.uses_sherpa = true;
  const nlohmann::json value = svp::builder::runtime_tools_json(selection);
  expect_equal(value.at("bundle").at("runtime_id").get<std::string>(), selection.runtime_id,
               "bundle runtime_id");
  expect_equal(value.at("ffmpeg").at("source").get<std::string>(), "bundled", "ffmpeg source");
  expect_equal(value.at("ffmpeg").at("blake3").get<std::string>(),
               "blake3:" + std::string(64, 'a'), "ffmpeg digest");
  expect_equal(value.at("ffprobe").at("source").get<std::string>(), "path_search",
               "ffprobe source");
  expect(!value.at("ffprobe").contains("blake3"), "no digest for PATH tools");
  // Nothing in this test loads sherpa-onnx.
  expect(value.at("sherpa_onnx").at("loaded") == false, "sherpa not loaded");

  const nlohmann::json empty = svp::builder::runtime_tools_json({});
  expect(empty.at("bundle").is_null(), "no bundle is recorded as null");
  expect(!empty.contains("ffmpeg") && !empty.contains("sherpa_onnx"),
         "tools the command does not use are omitted");
}

}  // namespace

int main() {
  const std::pair<const char*, void (*)()> tests[] = {
      {"no bundle keeps PATH defaults", test_no_bundle_keeps_path_defaults},
      {"bundle is used after flag and environment",
       test_bundle_is_used_after_flag_and_environment},
      {"bundle without a tool falls back to PATH",
       test_bundle_without_a_tool_falls_back_to_path},
      {"bundled sherpa library is offered to the search",
       test_bundled_sherpa_library_is_offered_to_the_search},
      {"broken bundle is an error", test_broken_bundle_is_an_error},
      {"runtime_id from manifest", test_runtime_id_from_manifest},
      {"selection JSON", test_selection_json},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "svp-builder-runtime-tools-tests: " << name << " FAILED: "
                << error.what() << "\n";
    }
  }
  return failures == 0 ? 0 : 1;
}
