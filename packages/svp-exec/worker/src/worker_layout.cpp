#include "svp/exec/worker/worker_layout.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <system_error>

namespace svp::exec::worker {

std::string_view worker_service_mode_name(WorkerServiceMode mode) noexcept {
  switch (mode) {
    case WorkerServiceMode::user_agent:
      return "user_agent";
    case WorkerServiceMode::system_daemon:
      return "system_daemon";
  }
  return "unknown";
}

std::optional<WorkerServiceMode> parse_worker_service_mode(std::string_view name) noexcept {
  for (const WorkerServiceMode mode :
       {WorkerServiceMode::user_agent, WorkerServiceMode::system_daemon}) {
    if (worker_service_mode_name(mode) == name) {
      return mode;
    }
  }
  return std::nullopt;
}

std::filesystem::path runtime_manifest_path(const std::filesystem::path& runtime_dir) {
  return runtime_dir / std::string(kRuntimeBundleDir) / "manifest.json";
}

std::filesystem::path runtime_components_path(const std::filesystem::path& runtime_dir) {
  return runtime_dir / std::string(kRuntimeBundleDir) / "components.json";
}

std::filesystem::path default_worker_root(WorkerServiceMode mode,
                                          const std::filesystem::path& home) {
  const std::filesystem::path library =
      mode == WorkerServiceMode::system_daemon ? std::filesystem::path("/Library")
                                               : home / "Library";
  return library / "Application Support" / "SVP" / "Worker";
}

std::filesystem::path launchd_plist_path(WorkerServiceMode mode,
                                         const std::filesystem::path& home,
                                         std::string_view label) {
  const std::string file = std::string(label) + ".plist";
  if (mode == WorkerServiceMode::system_daemon) {
    return std::filesystem::path("/Library/LaunchDaemons") / file;
  }
  return home / "Library" / "LaunchAgents" / file;
}

void create_worker_layout(const WorkerLayout& layout) {
  for (const std::filesystem::path& directory :
       {layout.root, layout.pairings(), layout.runtimes(), layout.models(), layout.cache(),
        layout.cas(), layout.incoming(), layout.sessions(), layout.logs()}) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      throw WorkerError(WorkerErrorCode::io,
                        "cannot create " + directory.string() + ": " + error.message());
    }
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
  }
}

void clear_worker_scratch(const WorkerLayout& layout) {
  for (const std::filesystem::path& directory : {layout.sessions(), layout.incoming()}) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
      std::error_code ignored;
      std::filesystem::remove_all(entry.path(), ignored);
    }
  }
}

}  // namespace svp::exec::worker
