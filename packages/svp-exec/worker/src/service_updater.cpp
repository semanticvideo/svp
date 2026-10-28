#include "svp/exec/worker/service_updater.hpp"

#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/worker/runtime_store.hpp"
#include "svp/exec/worker/service_link.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <system_error>
#include <vector>

namespace svp::exec::worker {
namespace {

std::uint64_t manifest_bytes(const RuntimeManifest& manifest) {
  std::uint64_t total = 0;
  for (const RuntimeManifestFile& file : manifest.files) {
    total += file.size_bytes;
  }
  return total;
}

std::string describe(const RuntimeRelease& release) {
  return blake3_prefixed(release.runtime_id) +
         (release.release_stamp ? " (release " + std::to_string(*release.release_stamp) + ")"
                                : std::string(" (unstamped)"));
}

}  // namespace

std::string_view update_step_name(UpdateStep step) noexcept {
  switch (step) {
    case UpdateStep::disabled:
      return "disabled";
    case UpdateStep::none:
      return "none";
    case UpdateStep::waiting_for_idle:
      return "waiting_for_idle";
    case UpdateStep::busy:
      return "busy";
    case UpdateStep::failed:
      return "failed";
    case UpdateStep::switched:
      return "switched";
  }
  return "unknown";
}

ServiceUpdater::ServiceUpdater(ServiceUpdaterOptions options) : options_(std::move(options)) {
  if (!options_.test_start) {
    options_.test_start = [](const std::filesystem::path& runtime_dir,
                             const Blake3Digest& runtime_id, std::uint64_t runtime_bytes) {
      return test_start_runtime(runtime_dir, runtime_id, test_start_deadline(runtime_bytes));
    };
  }
  if (!options_.launched_through_current) {
    disabled_reason_ = "the service was not started through " +
                       options_.layout.service_program().string();
  } else if (!options_.own_runtime) {
    disabled_reason_ = "the service's own runtime is unknown";
  } else {
    try {
      const std::filesystem::path own_dir = options_.layout.runtime(*options_.own_runtime);
      own_ = runtime_release_of(load_runtime_manifest(runtime_manifest_path(own_dir)), own_dir);
    } catch (const std::exception& error) {
      disabled_reason_ = std::string("the service's own runtime manifest is unreadable: ") +
                         error.what();
    }
  }
}

void ServiceUpdater::log(const std::string& line) const {
  if (options_.log) {
    options_.log(line);
  }
}

bool ServiceUpdater::try_enter_session() {
  const std::lock_guard lock(mutex_);
  if (testing_ || restarting_) {
    return false;
  }
  ++live_;
  return true;
}

void ServiceUpdater::leave_session() {
  {
    const std::lock_guard lock(mutex_);
    if (live_ > 0) {
      --live_;
    }
  }
  (void)consider();
}

void ServiceUpdater::runtime_installed() { (void)consider(); }

ServiceUpdateState ServiceUpdater::state() const {
  const std::lock_guard lock(mutex_);
  ServiceUpdateState state{.self_update = enabled(),
                           .release_stamp = own_.release_stamp,
                           .declined_runtimes = {failed_.begin(), failed_.end()},
                           .pending = std::nullopt};
  if (!enabled()) {
    return state;
  }
  const std::optional<Candidate> next = switching_ ? switching_ : newest_candidate();
  if (next && next->release.release_stamp) {
    state.pending = PendingServiceSwitch{.runtime_id = next->release.runtime_id,
                                         .release_stamp = *next->release.release_stamp,
                                         .bytes = next->bytes};
  }
  return state;
}

bool ServiceUpdater::restart_requested() const {
  const std::lock_guard lock(mutex_);
  return restarting_;
}

std::uint64_t ServiceUpdater::live_sessions() const {
  const std::lock_guard lock(mutex_);
  return live_;
}

std::optional<ServiceUpdater::Candidate> ServiceUpdater::newest_candidate() const {
  const WorkerRuntimeStore store(options_.layout.runtimes());
  std::optional<Candidate> best;
  for (const Blake3Digest& runtime_id : store.list()) {
    if (failed_.contains(runtime_id)) {
      continue;
    }
    RuntimeManifest manifest;
    try {
      manifest = load_runtime_manifest(runtime_manifest_path(store.directory_of(runtime_id)));
    } catch (const std::exception&) {
      continue;
    }
    const RuntimeRelease release = runtime_release_of(manifest, store.directory_of(runtime_id));
    if (is_newer_release(release, own_) &&
        (!best || is_newer_release(release, best->release))) {
      best = Candidate{.release = release, .bytes = manifest_bytes(manifest)};
    }
  }
  return best;
}

UpdateStep ServiceUpdater::consider() {
  if (!enabled()) {
    return UpdateStep::disabled;
  }
  bool any_failed = false;
  while (true) {
    Candidate candidate;
    {
      const std::lock_guard lock(mutex_);
      if (testing_ || restarting_) {
        return UpdateStep::busy;
      }
      const std::optional<Candidate> found = newest_candidate();
      if (!found) {
        return any_failed ? UpdateStep::failed : UpdateStep::none;
      }
      if (live_ > 0) {
        if (waiting_logged_ != found->release.runtime_id) {
          waiting_logged_ = found->release.runtime_id;
          log("self-update: runtime " + describe(found->release) + " is newer than " +
              describe(own_) + "; switching once no session is live (" +
              std::to_string(live_) + " live)");
        }
        return UpdateStep::waiting_for_idle;
      }
      testing_ = true;
      candidate = *found;
      switching_ = candidate;
    }

    const std::filesystem::path directory =
        options_.layout.runtime(candidate.release.runtime_id);
    std::string failure;
    try {
      (void)verify_runtime_directory(directory, candidate.release.runtime_id);
      const TestStartOutcome outcome =
          options_.test_start(directory, candidate.release.runtime_id, candidate.bytes);
      if (!outcome.passed) {
        failure = "test-start failed: " + outcome.reason;
      } else {
        point_current_at(options_.layout, candidate.release.runtime_id);
      }
    } catch (const std::exception& error) {
      failure = error.what();
    }

    {
      const std::lock_guard lock(mutex_);
      testing_ = false;
      if (!failure.empty()) {
        switching_.reset();
        failed_.insert(candidate.release.runtime_id);
        any_failed = true;
        log("self-update: staying on " + describe(own_) + "; runtime " +
            describe(candidate.release) + " was not adopted: " + failure);
        continue;
      }
      restarting_ = true;
    }
    log("self-update: " + options_.layout.current().string() + " now names runtime " +
        describe(candidate.release) + " (was " + describe(own_) +
        "); restarting with exit status " + std::to_string(kWorkerRestartExitCode));
    if (options_.request_restart) {
      options_.request_restart();
    }
    return UpdateStep::switched;
  }
}

RuntimePrune ServiceUpdater::prune_runtimes() {
  RuntimePrune prune;
  if (!enabled()) {
    return prune;
  }
  // Held throughout, so try_enter_session() waits for it.
  const std::lock_guard lock(mutex_);
  if (live_ > 0 || testing_ || restarting_) {
    return prune;
  }
  struct Installed {
    RuntimeRelease release;
    std::uint64_t bytes = 0;
  };
  const WorkerRuntimeStore store(options_.layout.runtimes());
  std::vector<Installed> older;
  for (const Blake3Digest& runtime_id : store.list()) {
    RuntimeManifest manifest;
    try {
      manifest = load_runtime_manifest(runtime_manifest_path(store.directory_of(runtime_id)));
    } catch (const std::exception&) {
      continue;
    }
    const RuntimeRelease release = runtime_release_of(manifest, store.directory_of(runtime_id));
    if (runtime_id != own_.runtime_id && !is_newer_release(release, own_)) {
      older.push_back(Installed{.release = release, .bytes = manifest_bytes(manifest)});
    }
  }
  const Installed* previous = nullptr;
  for (const Installed& runtime : older) {
    if (runtime.release.release_stamp &&
        (previous == nullptr || is_newer_release(runtime.release, previous->release))) {
      previous = &runtime;
    }
  }
  // Never the runtime launchd starts, whatever its release.
  const std::optional<Blake3Digest> current = read_current_runtime(options_.layout);
  for (const Installed& runtime : older) {
    if (&runtime == previous || runtime.release.runtime_id == current) {
      continue;
    }
    std::error_code error;
    std::filesystem::remove_all(store.directory_of(runtime.release.runtime_id), error);
    if (error) {
      log("runtime cleanup: cannot remove " + describe(runtime.release) + ": " +
          error.message());
      continue;
    }
    ++prune.removed_runtimes;
    prune.removed_bytes += runtime.bytes;
  }
  // Installs run only in sessions, and none is live.
  std::vector<std::filesystem::path> incomplete;
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(options_.layout.runtimes(), error)) {
    if (entry.path().filename().string().starts_with(kRuntimeIncomingPrefix)) {
      incomplete.push_back(entry.path());
    }
  }
  for (const std::filesystem::path& path : incomplete) {
    std::error_code removal;
    std::filesystem::remove_all(path, removal);
    if (!removal) {
      ++prune.removed_incomplete;
    }
  }
  if (prune.removed_runtimes > 0 || prune.removed_incomplete > 0) {
    log("runtime cleanup: removed " + std::to_string(prune.removed_runtimes) +
        " runtime(s) (" + std::to_string(prune.removed_bytes) + " bytes) and " +
        std::to_string(prune.removed_incomplete) + " unfinished install(s); kept " +
        describe(own_) + (previous ? " and " + describe(previous->release) : std::string()) +
        " and any newer");
  }
  return prune;
}

}  // namespace svp::exec::worker
