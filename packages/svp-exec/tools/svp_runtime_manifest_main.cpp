// svp-runtime-manifest: writes and verifies the runtime identity manifest of
// an installed SVP runtime (plan §3.2). `cmake --install` runs `write` after
// it has placed svp-builder and the runtime bundle; `verify` is the same check
// a worker performs before it runs a runtime.
//
//   svp-runtime-manifest write  --root PREFIX --bundle-dir REL
//                               [--add COMPONENT=REL ...]
//                               [--release-stamp UTC_SECONDS]
//   svp-runtime-manifest verify --root PREFIX --manifest FILE
//
// Both print the runtime_id on stdout. `write` first writes the runtime's
// release record, <bundle-dir>/release.json (runtime_release.hpp), with the
// current UTC time in seconds or --release-stamp to reproduce a given
// install, and lists it in the manifest like any other runtime file.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/runtime_manifest_assembly.hpp"
#include "svp/exec/runtime_release.hpp"

#include <charconv>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kUsageExitCode = 2;
constexpr int kFailureExitCode = 1;

int usage() {
  std::cerr << "usage:\n"
               "  svp-runtime-manifest write --root PREFIX --bundle-dir REL "
               "[--add COMPONENT=REL ...] [--release-stamp UTC_SECONDS]\n"
               "  svp-runtime-manifest verify --root PREFIX --manifest FILE\n";
  return kUsageExitCode;
}

struct Arguments {
  std::string command;
  std::filesystem::path root;
  std::string bundle_dir;
  std::filesystem::path manifest;
  std::vector<svp::exec::RuntimeManifestExtraFile> extra_files;
  std::optional<std::uint64_t> release_stamp;
};

std::optional<Arguments> parse_arguments(int argc, char** argv) {
  if (argc < 2) return std::nullopt;
  Arguments arguments;
  arguments.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view flag = argv[index];
    if (index + 1 >= argc) return std::nullopt;
    const std::string value = argv[++index];
    if (flag == "--root") {
      arguments.root = value;
    } else if (flag == "--bundle-dir") {
      arguments.bundle_dir = value;
    } else if (flag == "--manifest") {
      arguments.manifest = value;
    } else if (flag == "--release-stamp") {
      std::uint64_t stamp = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), stamp);
      if (error != std::errc{} || end != value.data() + value.size() || value.empty()) {
        return std::nullopt;
      }
      arguments.release_stamp = stamp;
    } else if (flag == "--add") {
      const std::size_t equals = value.find('=');
      if (equals == std::string::npos || equals == 0 || equals + 1 == value.size()) {
        return std::nullopt;
      }
      arguments.extra_files.push_back(
          {.component = value.substr(0, equals), .path = value.substr(equals + 1)});
    } else {
      return std::nullopt;
    }
  }
  if (arguments.root.empty()) return std::nullopt;
  return arguments;
}

// Writes next to the target and renames, so a reader never sees a partial file.
void write_file_atomically(const std::filesystem::path& path, const std::string& bytes) {
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.flush();
    if (!output) {
      throw std::runtime_error("cannot write " + temporary.string());
    }
  }
  std::filesystem::rename(temporary, path);
}

int run_write(const Arguments& arguments) {
  if (arguments.bundle_dir.empty()) return usage();
  const std::string release_path =
      arguments.bundle_dir + "/" + std::string(svp::exec::kRuntimeReleaseFileName);
  if (release_path != svp::exec::kRuntimeReleasePath) {
    std::cerr << "svp-runtime-manifest: warning: the release record is at " << release_path
              << ", not " << svp::exec::kRuntimeReleasePath
              << "; worker Macs will not move to this runtime by themselves\n";
  }
  write_file_atomically(arguments.root / release_path,
                        svp::exec::encode_runtime_release_record(
                            arguments.release_stamp ? *arguments.release_stamp
                                                    : svp::exec::release_stamp_now()));
  std::vector<svp::exec::RuntimeManifestExtraFile> extra_files = arguments.extra_files;
  extra_files.push_back({.component = std::string(svp::exec::kRuntimeReleaseComponent),
                         .path = release_path});
  const svp::exec::RuntimeManifest manifest =
      svp::exec::assemble_runtime_manifest(arguments.root, arguments.bundle_dir, extra_files);
  const std::filesystem::path output = arguments.root / arguments.bundle_dir /
                                       std::string(svp::exec::kRuntimeManifestFileName);
  write_file_atomically(output, svp::exec::encode_runtime_manifest(manifest));
  std::cout << svp::exec::blake3_prefixed(svp::exec::compute_runtime_id(manifest)) << "\n";
  return 0;
}

int run_verify(const Arguments& arguments) {
  if (arguments.manifest.empty()) return usage();
  const svp::exec::RuntimeManifest manifest =
      svp::exec::load_runtime_manifest(arguments.manifest);
  const auto findings = svp::exec::verify_runtime_files(manifest, arguments.root);
  for (const auto& finding : findings) {
    std::cerr << finding.path << ": "
              << svp::exec::runtime_file_problem_name(finding.problem) << " ("
              << finding.detail << ")\n";
  }
  std::cout << svp::exec::blake3_prefixed(svp::exec::compute_runtime_id(manifest)) << "\n";
  return findings.empty() ? 0 : kFailureExitCode;
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Arguments> arguments = parse_arguments(argc, argv);
  if (!arguments) return usage();
  try {
    if (arguments->command == "write") return run_write(*arguments);
    if (arguments->command == "verify") return run_verify(*arguments);
    return usage();
  } catch (const std::exception& error) {
    std::cerr << "svp-runtime-manifest: " << error.what() << "\n";
    return kFailureExitCode;
  }
}
