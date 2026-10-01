#include "worker_model_view.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/worker_error.hpp"

namespace svp::builder::workers {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

WorkerModelView::WorkerModelView(std::filesystem::path model_store,
                                 std::filesystem::path session_dir)
    : model_store_(std::move(model_store)), views_(std::move(session_dir) / "model-views") {}

std::filesystem::path WorkerModelView::cache_for(const svp::exec::TaskSpec& spec) {
  std::string key;
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    key += ref.model_id + "@" + svp::exec::blake3_hex(ref.bundle_blake3) + ";";
  }
  const std::lock_guard lock(mutex_);
  if (const auto found = views_by_refs_.find(key); found != views_by_refs_.end()) {
    return found->second;
  }
  const svp::exec::worker::WorkerModelStore store(model_store_);
  const std::filesystem::path view =
      views_ / svp::exec::blake3_hex(svp::exec::blake3_digest(key));
  std::error_code error;
  std::filesystem::create_directories(view, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io, "cannot create " + view.string());
  }
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    const std::string hex = svp::exec::blake3_hex(ref.bundle_blake3);
    if (!store.has(ref.bundle_blake3)) {
      throw WorkerError(WorkerErrorCode::configuration,
                        "model bundle " + ref.model_bundle_id + " is not installed on this worker");
    }
    if (!verified_.contains(hex)) {
      store.verify(ref.bundle_blake3);
      verified_.insert(hex);
    }
    const std::filesystem::path link = view / ref.model_id;
    std::filesystem::remove(link, error);
    std::filesystem::create_directory_symlink(store.directory_of(ref.bundle_blake3) / ref.model_id,
                                              link, error);
    if (error) {
      throw WorkerError(WorkerErrorCode::io, "cannot link " + link.string() + ": " +
                                                 error.message());
    }
  }
  views_by_refs_.emplace(key, view);
  return view;
}

}  // namespace svp::builder::workers
