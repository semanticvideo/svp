// svp-runtime-manifest: writes and verifies the runtime identity manifest of
// an installed SVP runtime (plan §3.2). `cmake --install` runs `write` after
// it has placed svp-builder and the runtime bundle; `verify` is the same check
// a worker performs before it runs a runtime.
//
//   svp-runtime-manifest write  --root PREFIX --bundle-dir REL
//                               [--add COMPONENT=REL ...]
//   svp-runtime-manifest verify --root PREFIX --manifest FILE
//
// Both print the runtime_id on stdout.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/runtime_manifest_assembly.hpp"

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
               "[--add COMPONENT=REL ...]\n"
               "  svp-runtime-manifest verify --root PREFIX --manifest FILE\n";
  return kUsageExitCode;
}

struct Arguments {
  std::string command;
  std::filesystem::path root;
  std::string bundle_dir;
  std::filesystem::path manifest;
  std::vector<svp::exec::RuntimeManifestExtraFile> extra_files;
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
  const svp::exec::RuntimeManifest manifest = svp::exec::assemble_runtime_manifest(
      arguments.root, arguments.bundle_dir, arguments.extra_files);
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
