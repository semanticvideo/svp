#pragma once

// Where a paired worker keeps everything SVP installs on it (plan §3.3),
// and how a runtime directory is laid out. Removing the root (plus the
// launchd job's plist) removes every trace of SVP from the worker.
//
//   <root>/                       0700, owned by the worker's user
//     current -> runtimes/<hex>   the runtime the launchd job runs: a
//                                 symlink with that RELATIVE target, owned
//                                 by the worker's user and swapped by the
//                                 service itself (service_link.hpp)
//     pairings/<pairing_id>.json  0600 pairing secrets (pairing_store.hpp)
//     join.json                   0600 fleet join credential, when the Mac
//                                 was installed with `worker install --join`
//                                 (fleet_store.hpp)
//     runtimes/<runtime hex>/     verified runtimes, one per runtime_id
//       bin/svp-builder           the session program (kSessionProgram)
//       libexec/svp/runtime/      manifest.json, and for a bundle runtime
//                                 components.json and the bundled tools
//     models/<bundle hex>/<model_id>/   verified model bundles
//     cache/cas/                  the worker's content-addressed store
//     cache/incoming/             blobs being received (per session)
//     cache/sessions/<id>/        session scratch, removed on disconnect and
//                                 at agent start
//     logs/agent.log              the launchd job's stdout and stderr
//
// Roots: a per-user LaunchAgent keeps it in the user's Library; a
// LaunchDaemon (--system-service) in the system Library, still owned by and
// run as the worker's user (plan §4.3: never as root).

#include "svp/exec/blake3_digest.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec::worker {

enum class WorkerServiceMode {
  // ~/Library/LaunchAgents, runs in the user's GUI session. Accepting
  // connections and advertising Bonjour need no Local Network permission
  // there (plan §3.3, Apple TN3179), but it runs only while that user is
  // logged in.
  user_agent,
  // /Library/LaunchDaemons with UserName set to the worker's user; runs with
  // nobody logged in. Installing it needs administrator rights (sudo).
  system_daemon,
};

[[nodiscard]] std::string_view worker_service_mode_name(WorkerServiceMode mode) noexcept;
[[nodiscard]] std::optional<WorkerServiceMode> parse_worker_service_mode(
    std::string_view name) noexcept;

// launchd label of the worker job; one per worker Mac, serving every
// pairing in its root.
inline constexpr std::string_view kWorkerJobLabel = "org.svp.worker";

// Runtime directory layout: the install prefix layout of cmake/SvpInstall.cmake
// (svp-builder in bin/, the bundle and its manifest in libexec/svp/runtime),
// so svp-builder run from a worker runtime finds its bundle exactly as an
// installed one does (svp/builder/runtime_tools.hpp).
inline constexpr std::string_view kRuntimeBundleDir = "libexec/svp/runtime";
inline constexpr std::string_view kSessionProgram = "bin/svp-builder";

[[nodiscard]] std::filesystem::path runtime_manifest_path(const std::filesystem::path& runtime_dir);
[[nodiscard]] std::filesystem::path runtime_components_path(
    const std::filesystem::path& runtime_dir);

// The worker root for `mode`; `home` is the worker user's home directory.
[[nodiscard]] std::filesystem::path default_worker_root(WorkerServiceMode mode,
                                                        const std::filesystem::path& home);
// Where the job's plist lives for `mode`.
[[nodiscard]] std::filesystem::path launchd_plist_path(WorkerServiceMode mode,
                                                       const std::filesystem::path& home,
                                                       std::string_view label = kWorkerJobLabel);

struct WorkerLayout {
  std::filesystem::path root;

  [[nodiscard]] std::filesystem::path current() const { return root / "current"; }
  // What the launchd job runs: <root>/current/bin/svp-builder.
  [[nodiscard]] std::filesystem::path service_program() const {
    return current() / std::string(kSessionProgram);
  }
  [[nodiscard]] std::filesystem::path pairings() const { return root / "pairings"; }
  [[nodiscard]] std::filesystem::path join_credential() const { return root / "join.json"; }
  [[nodiscard]] std::filesystem::path runtimes() const { return root / "runtimes"; }
  [[nodiscard]] std::filesystem::path runtime(const Blake3Digest& runtime_id) const {
    return runtimes() / blake3_hex(runtime_id);
  }
  [[nodiscard]] std::filesystem::path models() const { return root / "models"; }
  [[nodiscard]] std::filesystem::path cache() const { return root / "cache"; }
  [[nodiscard]] std::filesystem::path cas() const { return cache() / "cas"; }
  [[nodiscard]] std::filesystem::path incoming() const { return cache() / "incoming"; }
  [[nodiscard]] std::filesystem::path sessions() const { return cache() / "sessions"; }
  [[nodiscard]] std::filesystem::path logs() const { return root / "logs"; }
  [[nodiscard]] std::filesystem::path agent_log() const { return logs() / "agent.log"; }
};

// Creates every directory of the layout, private to this user (0700).
// Throws WorkerError(io).
void create_worker_layout(const WorkerLayout& layout);

// Removes leftover session scratch and partial incoming blobs (agent start).
void clear_worker_scratch(const WorkerLayout& layout);

}  // namespace svp::exec::worker
