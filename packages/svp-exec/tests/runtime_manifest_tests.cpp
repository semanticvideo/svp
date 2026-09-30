#include "storage_test_support.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/runtime_components.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/runtime_manifest_assembly.hpp"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

// The pinned runtime_id below was computed with an independent pure-Python
// BLAKE3 implementation over the documented identity bytes.

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSampleIdentityBytes =
    R"({"arch":"arm64","files":[)"
    R"({"blake3":"1111111111111111111111111111111111111111111111111111111111111111",)"
    R"("component":"svp-builder","path":"bin/svp-builder","size_bytes":1},)"
    R"({"blake3":"2222222222222222222222222222222222222222222222222222222222222222",)"
    R"("component":"ffmpeg","path":"libexec/svp/runtime/bin/ffmpeg","size_bytes":2}],)"
    R"("macos_deployment_target":"15.0","schema":"svp.runtime.manifest/1"})";
constexpr std::string_view kSampleRuntimeId =
    "b3:14c430ef59f78cad53eb13c292eb12730af86add0f34868a92ef782a22a76f90";

RuntimeManifest sample_manifest() {
  RuntimeManifest manifest;
  manifest.arch = "arm64";
  manifest.macos_deployment_target = "15.0";
  // Deliberately out of order: normalization owns the canonical order.
  manifest.files = {
      RuntimeManifestFile{.path = "libexec/svp/runtime/bin/ffmpeg",
                          .component = "ffmpeg",
                          .blake3 = repeated_digest(0x22),
                          .size_bytes = 2},
      RuntimeManifestFile{.path = "bin/svp-builder",
                          .component = "svp-builder",
                          .blake3 = repeated_digest(0x11),
                          .size_bytes = 1},
  };
  normalize_runtime_manifest(manifest);
  return manifest;
}

std::string with_runtime_id(std::string_view identity, std::string_view runtime_id) {
  // Canonical key order puts runtime_id between macos_deployment_target and
  // schema.
  std::string bytes(identity);
  const std::string anchor = R"(,"schema":)";
  bytes.insert(bytes.find(anchor), R"(,"runtime_id":")" + std::string(runtime_id) + "\"");
  return bytes;
}

void test_identity_bytes_and_runtime_id_vector() {
  const RuntimeManifest manifest = sample_manifest();
  expect_equal(runtime_manifest_identity_bytes(manifest), kSampleIdentityBytes,
               "identity bytes are the canonical manifest without runtime_id");
  expect_equal(blake3_prefixed(compute_runtime_id(manifest)), kSampleRuntimeId,
               "runtime_id vector");
  expect_equal(encode_runtime_manifest(manifest),
               with_runtime_id(kSampleIdentityBytes, kSampleRuntimeId),
               "on-disk form carries runtime_id in canonical key order");
}

void test_runtime_id_is_stable_and_content_sensitive() {
  RuntimeManifest reordered = sample_manifest();
  std::reverse(reordered.files.begin(), reordered.files.end());
  normalize_runtime_manifest(reordered);
  expect(compute_runtime_id(reordered) == compute_runtime_id(sample_manifest()),
         "input order does not change runtime_id");

  const auto changed_id = [](auto&& mutate) {
    RuntimeManifest manifest = sample_manifest();
    mutate(manifest);
    normalize_runtime_manifest(manifest);
    return compute_runtime_id(manifest);
  };
  const Blake3Digest baseline = compute_runtime_id(sample_manifest());
  expect(changed_id([](RuntimeManifest& m) { m.files[0].blake3[0] ^= 1; }) != baseline,
         "a file digest is part of runtime_id");
  expect(changed_id([](RuntimeManifest& m) { m.files[0].size_bytes += 1; }) != baseline,
         "a file size is part of runtime_id");
  expect(changed_id([](RuntimeManifest& m) { m.files[1].component = "ffprobe"; }) !=
             baseline,
         "a component name is part of runtime_id");
  expect(changed_id([](RuntimeManifest& m) { m.macos_deployment_target = "15.1"; }) !=
             baseline,
         "the macOS target is part of runtime_id");
  expect(changed_id([](RuntimeManifest& m) {
           m.files.push_back({.path = "libexec/svp/runtime/bin/ffprobe",
                              .component = "ffmpeg",
                              .blake3 = repeated_digest(0x33),
                              .size_bytes = 3});
         }) != baseline,
         "an added file changes runtime_id");
}

void test_decode_round_trip() {
  const RuntimeManifest manifest = sample_manifest();
  expect(decode_runtime_manifest(encode_runtime_manifest(manifest)) == manifest,
         "decode(encode(m)) == m");
}

void test_decode_rejects_invalid_manifests() {
  const std::string good = encode_runtime_manifest(sample_manifest());
  expect_exec_error(ExecErrorCode::non_canonical_json,
                    [&] { static_cast<void>(decode_runtime_manifest(good + "\n")); },
                    "trailing whitespace");

  std::string tampered_id = good;
  tampered_id[tampered_id.find("b3:") + 3] =
      tampered_id[tampered_id.find("b3:") + 3] == '0' ? '1' : '0';
  expect_exec_error(ExecErrorCode::digest_mismatch,
                    [&] { static_cast<void>(decode_runtime_manifest(tampered_id)); },
                    "runtime_id that does not match the content");

  std::string tampered_file = good;
  tampered_file.replace(tampered_file.find("\"size_bytes\":1"), 14, "\"size_bytes\":9");
  expect_exec_error(ExecErrorCode::digest_mismatch,
                    [&] { static_cast<void>(decode_runtime_manifest(tampered_file)); },
                    "file entry edited without a new runtime_id");

  expect_exec_error(ExecErrorCode::missing_field,
                    [] { static_cast<void>(decode_runtime_manifest(kSampleIdentityBytes)); },
                    "missing runtime_id");

  const auto rejects_manifest = [](ExecErrorCode code, auto&& mutate, std::string_view what) {
    nlohmann::json value = nlohmann::json::parse(encode_runtime_manifest(sample_manifest()));
    mutate(value);
    const std::string bytes = encode_canonical_json(value);
    expect_exec_error(code, [&] { static_cast<void>(decode_runtime_manifest(bytes)); }, what);
  };
  rejects_manifest(ExecErrorCode::unknown_field,
                   [](nlohmann::json& v) { v["extra"] = 1; }, "unknown field");
  rejects_manifest(ExecErrorCode::invalid_value,
                   [](nlohmann::json& v) { v["schema"] = "svp.runtime.manifest/2"; },
                   "unsupported schema");
  rejects_manifest(ExecErrorCode::invalid_value,
                   [](nlohmann::json& v) {
                     std::swap(v["files"][0], v["files"][1]);
                   },
                   "unsorted files");
  rejects_manifest(ExecErrorCode::invalid_value,
                   [](nlohmann::json& v) { v["files"][1] = v["files"][0]; },
                   "duplicate path");
  for (const char* unsafe : {"../bin/ffmpeg", "/usr/bin/ffmpeg", "bin//ffmpeg",
                             "bin/./ffmpeg", "bin/ffmpeg/", "bin\\ffmpeg", ""}) {
    rejects_manifest(ExecErrorCode::invalid_value,
                     [unsafe](nlohmann::json& v) { v["files"][0]["path"] = unsafe; },
                     std::string("unsafe path `") + unsafe + "`");
  }
}

// --- files on disk --------------------------------------------------------------

struct InstalledRuntime {
  TemporaryDirectory root{"svp-runtime-manifest"};
  RuntimeManifest manifest;

  InstalledRuntime() {
    write_file(root.path / "bin/svp-builder", "builder bytes");
    write_file(root.path / "libexec/svp/runtime/bin/ffmpeg", "ffmpeg bytes");
    manifest.arch = "arm64";
    manifest.macos_deployment_target = "15.0";
    manifest.files = {
        describe_runtime_file(root.path, "libexec/svp/runtime/bin/ffmpeg", "ffmpeg"),
        describe_runtime_file(root.path, "bin/svp-builder", "svp-builder"),
    };
    normalize_runtime_manifest(manifest);
  }

  [[nodiscard]] std::vector<RuntimeFileFinding> verify() const {
    return verify_runtime_files(manifest, root.path);
  }
};

void expect_single_finding(const std::vector<RuntimeFileFinding>& findings,
                           std::string_view path, RuntimeFileProblem problem,
                           std::string_view message) {
  expect(findings.size() == 1, std::string(message) + ": exactly one finding");
  expect_equal(findings[0].path, path, std::string(message) + ": path");
  expect_equal(runtime_file_problem_name(findings[0].problem),
               runtime_file_problem_name(problem), std::string(message) + ": problem");
}

void test_load_and_verify_installed_runtime() {
  const InstalledRuntime runtime;
  expect(runtime.manifest.files[0].size_bytes == std::string("builder bytes").size(),
         "describe records the file size");
  const fs::path manifest_path = runtime.root.path / "libexec/svp/runtime/manifest.json";
  write_file(manifest_path, encode_runtime_manifest(runtime.manifest));
  const RuntimeManifest loaded = load_runtime_manifest(manifest_path);
  expect(loaded == runtime.manifest, "load returns the written manifest");
  expect(runtime.verify().empty(), "an untouched runtime verifies");

  bool threw = false;
  try {
    static_cast<void>(load_runtime_manifest(runtime.root.path / "absent.json"));
  } catch (const std::system_error&) {
    threw = true;
  }
  expect(threw, "a missing manifest file is a system_error");
}

void test_verify_detects_tampered_files() {
  {
    const InstalledRuntime runtime;
    // Same size, different bytes: only the digest can catch it.
    write_file(runtime.root.path / "libexec/svp/runtime/bin/ffmpeg", "ffmpeg bytez");
    expect_single_finding(runtime.verify(), "libexec/svp/runtime/bin/ffmpeg",
                          RuntimeFileProblem::digest_mismatch, "same-size tamper");
  }
  {
    const InstalledRuntime runtime;
    write_file(runtime.root.path / "bin/svp-builder", "builder");
    expect_single_finding(runtime.verify(), "bin/svp-builder",
                          RuntimeFileProblem::size_mismatch, "truncated file");
  }
  {
    const InstalledRuntime runtime;
    fs::remove(runtime.root.path / "bin/svp-builder");
    expect_single_finding(runtime.verify(), "bin/svp-builder",
                          RuntimeFileProblem::missing, "removed file");
  }
  {
    const InstalledRuntime runtime;
    fs::remove(runtime.root.path / "bin/svp-builder");
    fs::create_directories(runtime.root.path / "bin/svp-builder");
    expect_single_finding(runtime.verify(), "bin/svp-builder",
                          RuntimeFileProblem::not_regular_file, "directory in place");
  }
}

// --- assembly from an installed bundle ------------------------------------------

std::string components_json(const RuntimeManifestFile& ffmpeg, std::string_view relative) {
  nlohmann::json value = {
      {"schema", std::string(kRuntimeComponentsSchema)},
      {"arch", "arm64"},
      {"macos_deployment_target", "15.0"},
      {"components",
       {{{"component", "ffmpeg"},
         {"version", "9.0.2"},
         {"files",
          {{{"path", std::string(relative)},
            {"blake3", "blake3:" + blake3_hex(ffmpeg.blake3)},
            {"size_bytes", ffmpeg.size_bytes}}}}}}},
      {"support_files", nlohmann::json::array()},
  };
  return value.dump(2);
}

void test_assemble_from_components_record() {
  const InstalledRuntime runtime;
  const fs::path bundle = runtime.root.path / "libexec/svp/runtime";
  const RuntimeManifestFile ffmpeg =
      describe_runtime_file(runtime.root.path, "libexec/svp/runtime/bin/ffmpeg", "ffmpeg");
  write_file(bundle / kRuntimeComponentsFileName, components_json(ffmpeg, "bin/ffmpeg"));

  const RuntimeManifest assembled = assemble_runtime_manifest(
      runtime.root.path, "libexec/svp/runtime",
      {{.component = "svp-builder", .path = "bin/svp-builder"}});
  expect(assembled == runtime.manifest,
         "assembly lists the bundle files under the bundle directory plus extras");

  RuntimeManifestFile stale = ffmpeg;
  stale.blake3[0] ^= 1;
  write_file(bundle / kRuntimeComponentsFileName, components_json(stale, "bin/ffmpeg"));
  expect_exec_error(ExecErrorCode::digest_mismatch,
                    [&] {
                      static_cast<void>(assemble_runtime_manifest(
                          runtime.root.path, "libexec/svp/runtime", {}));
                    },
                    "installed file differs from components.json");
}

void test_components_record_digest_form() {
  expect_exec_error(ExecErrorCode::invalid_digest,
                    [] {
                      static_cast<void>(parse_runtime_components(
                          R"({"schema":"svp.runtime.components/1","arch":"arm64",)"
                          R"("macos_deployment_target":"15.0","components":[)"
                          R"({"component":"ffmpeg","files":[{"path":"bin/ffmpeg",)"
                          R"("blake3":"b3:00","size_bytes":1}]}]})"));
                    },
                    "components.json digests must be blake3:<hex>");
}

}  // namespace

int main() {
  return run_tests("svp-exec-runtime-manifest-tests",
                   {
                       {"identity bytes and runtime_id vector",
                        test_identity_bytes_and_runtime_id_vector},
                       {"runtime_id is stable and content sensitive",
                        test_runtime_id_is_stable_and_content_sensitive},
                       {"decode round trip", test_decode_round_trip},
                       {"decode rejects invalid manifests",
                        test_decode_rejects_invalid_manifests},
                       {"load and verify installed runtime",
                        test_load_and_verify_installed_runtime},
                       {"verify detects tampered files", test_verify_detects_tampered_files},
                       {"assemble from components record",
                        test_assemble_from_components_record},
                       {"components record digest form", test_components_record_digest_form},
                   });
}
