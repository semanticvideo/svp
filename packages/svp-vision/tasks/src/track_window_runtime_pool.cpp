#include "svp/vision/tasks/track_window_runtime_pool.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace svp::vision::tasks {
namespace {

nlohmann::json threads_key(const svp::models::OrtThreadCounts& threads) {
  return {{"inter_op", threads.inter_op}, {"intra_op", threads.intra_op}};
}

// Every value load_visual_entity_window_runtimes reads.
std::string runtimes_key(const std::filesystem::path& model_cache_root,
                         const VisualEntityPipelineOptions& options) {
  const VisualEntityDetectorOptions detector = visual_entity_window_detector_options(options);
  const nlohmann::json key{
      {"model_cache_root", model_cache_root.string()},
      {"execution_provider", options.execution_provider},
      {"embedding_model_id", options.embedding_model_id},
      {"embedding_threads", threads_key(options.embedding_threads)},
      {"depth_threads", threads_key(options.depth_threads)},
      {"detector",
       {{"model_id", detector.model_id},
        {"execution_provider", detector.execution_provider},
        {"threads", threads_key(detector.threads)},
        {"confidence_threshold", detector.confidence_threshold},
        {"category_evidence_confidence_threshold",
         detector.category_evidence_confidence_threshold},
        {"nms_iou_threshold", detector.nms_iou_threshold},
        {"nms_containment_threshold", detector.nms_containment_threshold},
        {"cross_category_duplicate_iou_threshold",
         detector.cross_category_duplicate_iou_threshold},
        {"minimum_area_ratio", detector.minimum_area_ratio},
        {"maximum_area_ratio", detector.maximum_area_ratio},
        {"maximum_detections", detector.maximum_detections}}},
  };
  return key.dump();
}

}  // namespace

TrackWindowRuntimePool::Lease::Lease(TrackWindowRuntimePool& pool, std::string key,
                                     std::unique_ptr<VisualEntityWindowRuntimes> runtimes)
    : pool_(pool), key_(std::move(key)), runtimes_(std::move(runtimes)) {}

TrackWindowRuntimePool::Lease::~Lease() { pool_.release(key_, std::move(runtimes_)); }

std::unique_ptr<TrackWindowRuntimePool::Lease> TrackWindowRuntimePool::acquire(
    const std::filesystem::path& model_cache_root, const VisualEntityPipelineOptions& options) {
  std::string key = runtimes_key(model_cache_root, options);
  {
    const std::lock_guard lock(mutex_);
    auto idle = idle_.find(key);
    if (idle != idle_.end() && !idle->second.empty()) {
      std::unique_ptr<VisualEntityWindowRuntimes> runtimes = std::move(idle->second.back());
      idle->second.pop_back();
      return std::make_unique<Lease>(*this, std::move(key), std::move(runtimes));
    }
  }
  // Loading takes seconds; never under the lock.
  auto runtimes = std::make_unique<VisualEntityWindowRuntimes>(
      load_visual_entity_window_runtimes(model_cache_root, options));
  return std::make_unique<Lease>(*this, std::move(key), std::move(runtimes));
}

void TrackWindowRuntimePool::clear_idle() {
  std::map<std::string, std::vector<std::unique_ptr<VisualEntityWindowRuntimes>>> idle;
  {
    const std::lock_guard lock(mutex_);
    idle.swap(idle_);
  }
}

void TrackWindowRuntimePool::release(const std::string& key,
                                     std::unique_ptr<VisualEntityWindowRuntimes> runtimes) {
  if (runtimes == nullptr || !visual_entity_window_runtimes_complete(*runtimes)) return;
  const std::lock_guard lock(mutex_);
  idle_[key].push_back(std::move(runtimes));
}

}  // namespace svp::vision::tasks
