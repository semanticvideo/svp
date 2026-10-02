#pragma once

#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_window.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace svp::vision::tasks {

// Window runtimes (detector, depth, embedding sessions) kept alive across
// track.window tasks in one process. Loading and verifying the three bundles
// costs seconds and about a gigabyte (track_window_spec.hpp), so a set is
// reused by every later task with the same settings instead of being rebuilt
// per window. Reuse cannot change output: a set is keyed by every value that
// shapes it (model cache, model IDs, execution provider, per-model threads,
// detector thresholds), and a window's only use of runtime state, the
// detector's counters, is reported per window as a difference.
//
// Each concurrently running task holds its own set, so the pool grows to at
// most the number of tasks the process runs at once. Sets that did not load
// completely are never kept, so a later task retries the load. Thread-safe.
class TrackWindowRuntimePool {
 public:
  class Lease {
   public:
    Lease(TrackWindowRuntimePool& pool, std::string key,
          std::unique_ptr<VisualEntityWindowRuntimes> runtimes);
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    [[nodiscard]] VisualEntityWindowRuntimes& runtimes() { return *runtimes_; }

   private:
    TrackWindowRuntimePool& pool_;
    std::string key_;
    std::unique_ptr<VisualEntityWindowRuntimes> runtimes_;
  };

  // An idle set loaded with the same settings, or a new one.
  [[nodiscard]] std::unique_ptr<Lease> acquire(const std::filesystem::path& model_cache_root,
                                               const VisualEntityPipelineOptions& options);

  // Destroys every idle set (their memory is returned). A coordinator calls
  // it once its tracking stage has reduced.
  void clear_idle();

 private:
  void release(const std::string& key, std::unique_ptr<VisualEntityWindowRuntimes> runtimes);

  std::mutex mutex_;
  std::map<std::string, std::vector<std::unique_ptr<VisualEntityWindowRuntimes>>> idle_;
};

}  // namespace svp::vision::tasks
