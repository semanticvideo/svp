#include "vision_lane_placeholders.hpp"

#include "vision_lane_files.hpp"

namespace svp::package::detail {
namespace {

constexpr const char* kSpatialPlaceholderProcessorId =
    "processor_spatial_placeholder_0001";
constexpr const char* kEmbeddingPlaceholderProcessorId =
    "processor_embedding_placeholder_0001";

}  // namespace

nlohmann::json make_spatial_placeholder_processor() {
  return {
      {"id", kSpatialPlaceholderProcessorId},
      {"name", "svp spatial placeholder writer"},
      {"version", "svp-spatial-placeholder-v1"},
      {"input_refs", nlohmann::json::array()},
      {"output_refs", {
          "spatial/depth.index.jsonl",
          "spatial/depth.blocks.svpdz",
          "spatial/masks.index.jsonl",
          "spatial/masks.blocks.svpmz"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.spatial.placeholder"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", "not_run"},
      {"note", "Depth estimation and mask generation have not been run. "
               "Empty index files, a zero-length mask block stream, and a "
               "zero-length depth block stream are written as honest "
               "placeholders. The validator requires real depth blocks "
               "with frame ranges; the empty depth block stream will be "
               "reported as invalid until real depth estimation is run. "
               "Depth requires Depth Anything V2 Small via ONNX Runtime, "
               "which is not configured in this build."}
  };
}

nlohmann::json make_embedding_placeholder_processor() {
  return {
      {"id", kEmbeddingPlaceholderProcessorId},
      {"name", "svp embedding placeholder writer"},
      {"version", "svp-embedding-placeholder-v1"},
      {"input_refs", nlohmann::json::array()},
      {"output_refs", {
          "embeddings/embedding_sets.json",
          "embeddings/embeddings.index.jsonl",
          "embeddings/embeddings.blocks.svpez"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.embeddings.placeholder"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", "not_run"},
      {"note", "Embedding generation has not been run. An empty embedding "
               "sets file, empty embedding index, and a zero-length "
               "embedding block stream are written as honest placeholders. "
               "The validator requires real embedding blocks with model "
               "output; the empty embedding block stream will be reported "
               "as invalid until real model inference is run. Embeddings "
               "require Nomic text/vision models via ONNX Runtime, which "
               "is not configured in this build."}
  };
}

void write_depth_placeholder_files(const std::filesystem::path& staging_dir) {
  write_empty_file(staging_dir / "spatial" / "depth.index.jsonl");
  write_empty_file(staging_dir / "spatial" / "depth.blocks.svpdz");
}

void write_mask_placeholder_files(const std::filesystem::path& staging_dir) {
  write_empty_file(staging_dir / "spatial" / "masks.index.jsonl");
  write_empty_file(staging_dir / "spatial" / "masks.blocks.svpmz");
}

void write_embedding_placeholder_files(const std::filesystem::path& staging_dir) {
  write_text_file(staging_dir / "embeddings" / "embedding_sets.json",
                  "{\"sets\": []}\n");
  write_empty_file(staging_dir / "embeddings" / "embeddings.index.jsonl");
  write_empty_file(staging_dir / "embeddings" / "embeddings.blocks.svpez");
}

}  // namespace svp::package::detail
