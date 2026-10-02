#include "engine/distributed_vision_work.hpp"

#include "svp/vision/depth_generation.hpp"
#include "svp/vision/embedding_generation.hpp"
#include "svp/vision/tasks/model_refs.hpp"

namespace svp::builder::engine {
namespace {

std::optional<DistributedOnnxWork> onnx_work(const std::filesystem::path& model_cache_root,
                                             const std::string& model_id,
                                             const std::string& execution_provider,
                                             const svp::models::OrtThreadCounts& threads) {
  if (threads.intra_op < 1 || threads.inter_op < 1) {
    return std::nullopt;
  }
  try {
    return DistributedOnnxWork{
        .model_ref = svp::vision::tasks::cached_model_ref(model_cache_root, model_id),
        .model = svp::vision::DispatchedModel{.model_id = model_id,
                                              .execution_provider = execution_provider,
                                              .threads = threads}};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

DistributedVisionWork plan_distributed_vision_work(const std::filesystem::path& model_cache_root,
                                                   const svp::models::ThreadPlan& thread_plan) {
  const svp::vision::EmbeddingGenerationOptions embedding;
  const svp::vision::DepthGenerationOptions depth;
  DistributedVisionWork work;
  work.evidence_crops = true;
  work.embedding_dim = embedding.embedding_dim;
  work.text_embeddings = onnx_work(model_cache_root, embedding.text_model_id,
                                   embedding.execution_provider, thread_plan.text_embedding);
  work.keyframe_embeddings =
      onnx_work(model_cache_root, embedding.vision_model_id, embedding.execution_provider,
                thread_plan.visual_entity_embedding);
  work.depth =
      onnx_work(model_cache_root, depth.model_id, depth.execution_provider, thread_plan.depth);
  return work;
}

}  // namespace svp::builder::engine
