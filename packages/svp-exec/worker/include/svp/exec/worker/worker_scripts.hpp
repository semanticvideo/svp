#pragma once

// POSIX shell scripts the coordinator runs on a worker over the system ssh
// during `workers pair` and `workers unpair` (plan §3.3). They are generated
// here, as text, so they can be unit-tested and printed by a dry run; the
// only data they receive on stdin is file content (a runtime tar stream, a
// pairing record, a plist), never a secret on a command line.
//
// Every script is run as `/bin/sh -c '<script>'` by the worker's user, except
// the system-daemon install and uninstall scripts, which are staged as files
// and run with `sudo /bin/sh <file>` in a terminal session so sudo can ask
// for the administrator password itself.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec::worker {

// Single-quotes `text` for /bin/sh.
[[nodiscard]] std::string shell_quote(std::string_view text);

// Read-only facts about the worker, printed as key=value lines.
[[nodiscard]] std::string render_probe_script(std::string_view label = kWorkerJobLabel);

struct WorkerProbe {
  std::string arch;
  std::string product_version;
  std::string build;
  std::string user;
  std::uint32_t uid = 0;
  std::string home;
  std::uint64_t home_available_bytes = 0;
  std::uint64_t library_available_bytes = 0;
  // Pairing records already present under each mode's root.
  std::uint64_t user_agent_pairings = 0;
  std::uint64_t system_daemon_pairings = 0;
  bool user_agent_job_loaded = false;
  bool system_daemon_plist_present = false;
};

// Throws WorkerError(command) naming the first missing or malformed key.
[[nodiscard]] WorkerProbe parse_probe_output(std::string_view output);

// Extracts a tar stream from stdin into `<runtimes_dir>/<hex>` after the
// runtime's own svp-builder verified every file against its manifest
// (`svp-builder worker verify-runtime`). An existing copy is kept when it
// verifies and replaced when it does not. Prints "runtime_id=b3:<hex>".
[[nodiscard]] std::string render_receive_runtime_script(const std::filesystem::path& runtimes_dir,
                                                        const Blake3Digest& runtime_id);

// Writes stdin to `path` (atomic rename) with `mode` (octal, e.g. 0600),
// creating parent directories private to the user.
[[nodiscard]] std::string render_write_file_script(const std::filesystem::path& path,
                                                   unsigned mode);

// Creates the layout directories under `root` (0700).
[[nodiscard]] std::string render_prepare_root_script(const std::filesystem::path& root);

// Creates the per-user LaunchAgents directory when it is missing (0755, as
// macOS creates it) and then leaves kCreatedLaunchAgentsMarker in `root` so
// removal can take the directory away again. Runs before the plist is
// written.
[[nodiscard]] std::string render_prepare_launch_agents_script(const std::filesystem::path& plist,
                                                              const std::filesystem::path& root);

// Loads (or reloads) the per-user LaunchAgent whose plist is already in
// place, then waits until launchd reports it running.
[[nodiscard]] std::string render_agent_start_script(const std::filesystem::path& plist,
                                                    std::string_view label = kWorkerJobLabel);

struct DaemonInstall {
  std::string user;
  // The worker user's staging directory holding runtimes/<hex>/,
  // pairings/<id>.json, and <label>.plist.
  std::filesystem::path staging;
  std::filesystem::path root;
  std::filesystem::path plist;
  Blake3Digest runtime_id{};
  std::string label = std::string(kWorkerJobLabel);
};

// Run with sudo: moves the staged runtime and pairing into the system root
// (owned by the worker's user), installs the plist root:wheel 0644, and
// bootstraps the daemon into the system domain.
[[nodiscard]] std::string render_daemon_install_script(const DaemonInstall& install);

struct WorkerRemoval {
  WorkerServiceMode mode = WorkerServiceMode::user_agent;
  std::filesystem::path root;
  std::filesystem::path plist;
  std::string pairing_id;
  std::string label = std::string(kWorkerJobLabel);
};

// Removes one pairing. When it was the last one, also stops and removes the
// job, its plist, and the whole root (runtimes, models, cache, logs);
// otherwise restarts the job so it stops serving the removed pairing.
// Prints "removed=all" or "removed=pairing remaining=<n>". The
// system_daemon variant must run with sudo.
[[nodiscard]] std::string render_removal_script(const WorkerRemoval& removal);

// Marker the agent install leaves in the root when it had to create
// ~/Library/LaunchAgents, so removal can take that directory away again.
inline constexpr std::string_view kCreatedLaunchAgentsMarker = ".created-launch-agents-dir";

}  // namespace svp::exec::worker
