#include "svp/exec/worker/runtime_source.hpp"

#include "runtime_files.hpp"
#include "svp/exec/runtime_manifest_assembly.hpp"
#include "svp/exec/runtime_release.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace svp::exec::worker {
namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw WorkerError(WorkerErrorCode::io, "cannot read " + path.string());
  }
  std::ostringstream bytes;
  bytes << stream.rdbuf();
  return bytes.str();
}

std::string compiled_arch() {
#if defined(__aarch64__)
  return "arm64";
#elif defined(__x86_64__)
  return "x86_64";
#else
  return "unknown";
#endif
}

void finish(CoordinatorRuntime& runtime) {
  runtime.runtime_id = compute_runtime_id(runtime.manifest);
  runtime.manifest_bytes = encode_runtime_manifest(runtime.manifest);
}

}  // namespace

std::string compiled_macos_deployment_target() {
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
  constexpr int kVersion = __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__;
  // macOS 11 and later encode MMmmpp; 10.x encodes 10mmpp as well.
  return std::to_string(kVersion / 10000) + "." + std::to_string((kVersion / 100) % 100);
#else
  return "unknown";
#endif
}

CoordinatorRuntime single_program_runtime(const std::filesystem::path& program,
                                          const std::string& component,
                                          std::string fallback_reason) {
  CoordinatorRuntime runtime;
  runtime.kind = RuntimeKind::builder_only;
  runtime.fallback_reason = std::move(fallback_reason);
  RuntimeManifestFile file;
  try {
    file = describe_runtime_file(program.parent_path(), program.filename().string(), component);
  } catch (const std::exception& error) {
    throw WorkerError(WorkerErrorCode::io, "cannot describe " + program.string() + ": " +
                                               error.what());
  }
  file.path = std::string(kSessionProgram);
  runtime.manifest.arch = compiled_arch();
  runtime.manifest.macos_deployment_target = compiled_macos_deployment_target();
  runtime.manifest.files.push_back(file);
  normalize_runtime_manifest(runtime.manifest);
  runtime.files.push_back(RuntimeFileSource{
      .path = file.path,
      .blob = BlobSource{.ref = BlobRef{.blake3 = file.blake3, .bytes = file.size_bytes},
                         .file = program}});
  finish(runtime);
  return runtime;
}

CoordinatorRuntime locate_coordinator_runtime(const std::filesystem::path& executable) {
  std::error_code error;
  const std::filesystem::path program = std::filesystem::weakly_canonical(executable, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io, "cannot resolve " + executable.string());
  }
  const std::filesystem::path prefix = program.parent_path().parent_path();
  const std::filesystem::path manifest_file = runtime_manifest_path(prefix);
  const std::filesystem::path components_file = runtime_components_path(prefix);
  const bool installed_layout = program.parent_path().filename() == "bin";
  if (!installed_layout || !std::filesystem::exists(manifest_file, error) ||
      !std::filesystem::exists(components_file, error)) {
    return single_program_runtime(
        program, "svp-builder",
        "no runtime bundle is installed next to " + program.string() +
            "; workers run svp-builder alone and find ffmpeg, ffprobe, and sherpa-onnx the "
            "way a local build without a bundle does");
  }

  CoordinatorRuntime runtime;
  runtime.kind = RuntimeKind::bundle;
  try {
    runtime.manifest = load_runtime_manifest(manifest_file);
  } catch (const std::exception& failure) {
    throw WorkerError(WorkerErrorCode::verification,
                      "runtime manifest " + manifest_file.string() + ": " + failure.what());
  }
  const std::vector<RuntimeFileFinding> findings = verify_runtime_files(runtime.manifest, prefix);
  if (!findings.empty()) {
    throw WorkerError(WorkerErrorCode::verification,
                      "installed runtime " + prefix.string() + " does not match its manifest: " +
                          findings.front().path + " " +
                          std::string(runtime_file_problem_name(findings.front().problem)));
  }
  bool has_program = false;
  for (const RuntimeManifestFile& file : runtime.manifest.files) {
    has_program = has_program || file.path == kSessionProgram;
    runtime.files.push_back(RuntimeFileSource{
        .path = file.path,
        .blob = BlobSource{.ref = BlobRef{.blake3 = file.blake3, .bytes = file.size_bytes},
                           .file = prefix / file.path}});
  }
  if (!has_program) {
    throw WorkerError(WorkerErrorCode::verification,
                      "runtime manifest " + manifest_file.string() + " does not list " +
                          std::string(kSessionProgram));
  }
  runtime.components_bytes = read_file(components_file);
  runtime.release_stamp = runtime_release_of(runtime.manifest, prefix).release_stamp;
  finish(runtime);
  return runtime;
}

void stage_runtime_directory(const CoordinatorRuntime& runtime,
                             const std::filesystem::path& directory) {
  std::error_code error;
  if (std::filesystem::exists(directory, error)) {
    throw WorkerError(WorkerErrorCode::io, directory.string() + " already exists");
  }
  for (const RuntimeFileSource& file : runtime.files) {
    const std::filesystem::path target = directory / file.path;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error || !std::filesystem::copy_file(file.blob.file, target, error) || error) {
      throw WorkerError(WorkerErrorCode::io, "cannot stage " + file.blob.file.string() + ": " +
                                                 error.message());
    }
    std::filesystem::permissions(target, detail::runtime_file_mode(file.path),
                                 std::filesystem::perm_options::replace, error);
  }
  detail::write_text_file(runtime_manifest_path(directory), runtime.manifest_bytes);
  if (runtime.components_bytes) {
    detail::write_text_file(runtime_components_path(directory), *runtime.components_bytes);
  }
}

}  // namespace svp::exec::worker
