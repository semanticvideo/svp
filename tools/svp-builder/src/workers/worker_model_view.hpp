#pragma once

// A worker session's model cache for one task (plan §4.1 `model_set`): a
// directory holding, under each model_id, a link to the verified bundle the
// task's model_refs name in the worker's content-addressed model store
// (<worker root>/models/<bundle_blake3>/<model_id>, model_bundles.hpp). So a
// task loads exactly the bundles the coordinator named, even when the worker
// holds several bundles of one model for different coordinators. Each bundle
// is re-verified against its manifest the first time a session uses it
// ("before use", WorkerModelStore::verify). Views live in the session's
// scratch directory and go with it. Thread-safe.

#include "svp/exec/task_spec.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace svp::builder::workers {

class WorkerModelView {
 public:
  WorkerModelView(std::filesystem::path model_store, std::filesystem::path session_dir);

  // The model cache for `spec`. Throws svp::exec::worker::WorkerError when a
  // named bundle is not installed or does not verify.
  [[nodiscard]] std::filesystem::path cache_for(const svp::exec::TaskSpec& spec);

 private:
  std::filesystem::path model_store_;
  std::filesystem::path views_;
  std::mutex mutex_;
  std::set<std::string> verified_;
  std::map<std::string, std::filesystem::path> views_by_refs_;
};

}  // namespace svp::builder::workers
