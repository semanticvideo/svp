#pragma once

// The coordinator's runtime as something that can be pushed to a worker
// (plan §3.2, §3.3: "the coordinator's runtime is used everywhere in a
// build"). Two shapes:
//
//   bundle        svp-builder is installed with a runtime bundle next to it
//                 (<prefix>/bin/svp-builder and <prefix>/libexec/svp/runtime
//                 with manifest.json and components.json). The manifest
//                 already names every file; all of them are verified here
//                 before anything is pushed.
//   builder_only  no bundle is installed (for example svp-builder run from a
//                 build tree). The runtime is svp-builder alone, under
//                 kSessionProgram, with a manifest assembled here; workers
//                 then resolve ffmpeg, ffprobe, and sherpa-onnx the way a
//                 local build without a bundle does. RuntimeKind records it.
//
// Either way the worker receives the same directory layout
// (worker_layout.hpp) and verifies it against the manifest before running it.

#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/worker/blob_source.hpp"
#include "svp/exec/worker/hello_messages.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::exec::worker {

struct RuntimeFileSource {
  // Path inside the runtime (the manifest's `path`).
  std::string path;
  BlobSource blob;
};

struct CoordinatorRuntime {
  RuntimeKind kind = RuntimeKind::bundle;
  RuntimeManifest manifest;
  Blake3Digest runtime_id{};
  // Canonical manifest bytes (encode_runtime_manifest) and, for a bundle,
  // components.json exactly as installed.
  std::string manifest_bytes;
  std::optional<std::string> components_bytes;
  std::vector<RuntimeFileSource> files;
  // Why the runtime is builder_only, for reports; empty for a bundle.
  std::string fallback_reason;
};

// The runtime of the svp-builder at `executable` (its real path). Throws
// WorkerError(verification) when an installed bundle does not match its
// manifest, and WorkerError(io) when a file cannot be read.
[[nodiscard]] CoordinatorRuntime locate_coordinator_runtime(
    const std::filesystem::path& executable);

// A builder_only runtime whose kSessionProgram is `program`, recorded under
// `component` (tests use it to ship a stand-in session program).
[[nodiscard]] CoordinatorRuntime single_program_runtime(const std::filesystem::path& program,
                                                        const std::string& component,
                                                        std::string fallback_reason);

// Lays the runtime out under `directory` exactly as a worker stores it:
// every file at its manifest path (executables 0755, others 0644), the
// manifest, and components.json when there is one. `directory` must not
// exist yet. Throws WorkerError(io).
void stage_runtime_directory(const CoordinatorRuntime& runtime,
                             const std::filesystem::path& directory);

// The manifest's macOS deployment target for runtimes assembled here: the
// minimum macOS this build of svp-builder was compiled for.
[[nodiscard]] std::string compiled_macos_deployment_target();

}  // namespace svp::exec::worker
