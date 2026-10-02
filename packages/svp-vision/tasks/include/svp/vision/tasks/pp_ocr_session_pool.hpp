#pragma once

#include "svp/vision/pp_ocr.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace svp::vision::tasks {

// PP-OCR sessions kept alive across ocr.frame_batch tasks in one worker
// process. Loading the detector and recognizer costs seconds and ~1.2-1.5 GB
// (plan §2.3), so a session is reused by every later task with the same
// session settings instead of being rebuilt per batch. Reuse cannot change
// output: a session is keyed by every value that shapes it (model IDs,
// execution provider, per-model threads, graph optimization, execution
// mode), and per-frame values (thresholds, sizes) are passed on every call.
//
// The coordinator shares one pool between its frame-batch tasks and the OCR
// reducer (whose evidence-crop re-read needs the same recognizer), so a
// build holds one set of sessions per concurrent OCR task, not one more for
// the reducer.
//
// Each concurrently running task holds its own session, so the pool grows to
// at most the number of tasks the worker runs at once, which the scheduler
// bounds by the worker's advertised slots and memory. Idle sessions live
// until the pool (the worker session) ends. Sessions that failed to load are
// never kept, so a later task retries the load. Thread-safe.
class PpOcrSessionPool {
 public:
  PpOcrSessionPool() = default;

  class Lease {
   public:
    Lease(PpOcrSessionPool& pool, std::string key, std::unique_ptr<PpOcrSession> session);
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    [[nodiscard]] const PpOcrSession& session() const { return *session_; }

   private:
    PpOcrSessionPool& pool_;
    std::string key_;
    std::unique_ptr<PpOcrSession> session_;
  };

  // An idle session created with the same session settings (the model cache
  // root included), or a new one.
  [[nodiscard]] std::unique_ptr<Lease> acquire(const PpOcrOptions& options);

  // Destroys every idle session (their memory is returned); leased sessions
  // are unaffected. A coordinator calls it once its OCR stage has reduced,
  // so later stages do not run beside sessions nothing will use again.
  void clear_idle();

 private:
  void release(const std::string& key, std::unique_ptr<PpOcrSession> session);

  std::mutex mutex_;
  std::map<std::string, std::vector<std::unique_ptr<PpOcrSession>>> idle_;
};

}  // namespace svp::vision::tasks
