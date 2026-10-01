#include "pp_ocr_session_pool.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace svp::vision::tasks {
namespace {

// Every PpOcrOptions value create_pp_ocr_session reads.
std::string session_key(const PpOcrOptions& options) {
  const nlohmann::json key{
      {"det_execution_mode", options.det_execution_mode},
      {"det_graph_optimization_level", options.det_graph_optimization_level},
      {"det_inter_op", options.det_threads.inter_op},
      {"det_intra_op", options.det_threads.intra_op},
      {"detector_model_id", options.detector_model_id},
      {"execution_provider", options.execution_provider},
      {"manifest_filename", options.manifest_filename},
      {"rec_execution_mode", options.rec_execution_mode},
      {"rec_graph_optimization_level", options.rec_graph_optimization_level},
      {"rec_inter_op", options.rec_threads.inter_op},
      {"rec_intra_op", options.rec_threads.intra_op},
      {"recognizer_model_id", options.recognizer_model_id},
  };
  return key.dump();
}

}  // namespace

PpOcrSessionPool::PpOcrSessionPool(std::filesystem::path model_cache_root)
    : model_cache_root_(std::move(model_cache_root)) {}

PpOcrSessionPool::Lease::Lease(PpOcrSessionPool& pool, std::string key,
                               std::unique_ptr<PpOcrSession> session)
    : pool_(pool), key_(std::move(key)), session_(std::move(session)) {}

PpOcrSessionPool::Lease::~Lease() {
  pool_.release(key_, std::move(session_));
}

PpOcrOptions PpOcrSessionPool::with_model_cache(PpOcrOptions options) const {
  options.model_cache_root = model_cache_root_;
  return options;
}

std::unique_ptr<PpOcrSessionPool::Lease> PpOcrSessionPool::acquire(
    const PpOcrOptions& options) {
  const PpOcrOptions resolved = with_model_cache(options);
  std::string key = session_key(resolved);
  {
    const std::lock_guard lock(mutex_);
    auto idle = idle_.find(key);
    if (idle != idle_.end() && !idle->second.empty()) {
      std::unique_ptr<PpOcrSession> session = std::move(idle->second.back());
      idle->second.pop_back();
      return std::make_unique<Lease>(*this, std::move(key), std::move(session));
    }
  }
  // Loading takes seconds; never under the lock.
  auto session = std::make_unique<PpOcrSession>(create_pp_ocr_session(resolved));
  return std::make_unique<Lease>(*this, std::move(key), std::move(session));
}

void PpOcrSessionPool::release(const std::string& key,
                               std::unique_ptr<PpOcrSession> session) {
  if (session == nullptr || !session->available) return;
  const std::lock_guard lock(mutex_);
  idle_[key].push_back(std::move(session));
}

}  // namespace svp::vision::tasks
