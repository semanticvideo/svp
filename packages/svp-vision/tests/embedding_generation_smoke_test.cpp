#include "svp/vision/embedding_generation.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  const std::filesystem::path model_cache =
      std::filesystem::path("/Users/domesposito/Projects/svp-model-cache");

  if (!std::filesystem::exists(model_cache / "model_nomic_embed_text_v1_5")) {
    std::cerr << "Skipping: model cache not found at " << model_cache << "\n";
    return 0;
  }

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-embed-gen-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");

  // Seed text observations
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << R"({"text_observation_id":"text_obs_000001","text_region_id":"text_region_000001","observation_type":"text_recognition","raw_text":"SALE $9.99","normalized_text":"sale 9.99","confidence":0.92,"provenance_id":"processor_ocr_recognizer_0001"})" << "\n";
    out << R"({"text_observation_id":"text_obs_000002","text_region_id":"text_region_000002","observation_type":"text_recognition","raw_text":"Welcome to our store","normalized_text":"welcome to our store","confidence":0.88,"provenance_id":"processor_ocr_recognizer_0001"})" << "\n";
  }

  svp::vision::EmbeddingGenerationOptions opts;
  opts.model_cache_root = model_cache;
  opts.text_model_id = "model_nomic_embed_text_v1_5";
  opts.execution_provider = "cpu";
  opts.embedding_dim = 768;

  svp::vision::EmbeddingGenerationResult result =
      svp::vision::generate_embedding_blocks(opts, staging_dir);

  std::cout << "onnx_runtime_available: " << result.onnx_runtime_available << "\n";
  std::cout << "embedding_model_available: " << result.embedding_model_available << "\n";
  std::cout << "embedding_generation_run: " << result.embedding_generation_run << "\n";
  std::cout << "embeddings_blocks_written: " << result.embeddings_blocks_written << "\n";
  std::cout << "embeddings_index_written: " << result.embeddings_index_written << "\n";
  std::cout << "embedding_sets_written: " << result.embedding_sets_written << "\n";
  std::cout << "model_id: " << result.model_id << "\n";
  std::cout << "model_bundle_id: " << result.model_bundle_id << "\n";
  std::cout << "blocker: " << result.blocker << "\n";
  std::cout << "entries: " << result.entries.size() << "\n";

  assert(result.onnx_runtime_available);
  assert(result.embedding_model_available);
  assert(result.embedding_generation_run);
  assert(result.embeddings_blocks_written);
  assert(result.embeddings_index_written);
  assert(result.embedding_sets_written);
  assert(result.entries.size() == 2);
  assert(result.blocker.empty());

  // Verify block file is non-empty
  const auto blocks_path = staging_dir / "embeddings" / "embeddings.blocks.svpez";
  assert(std::filesystem::exists(blocks_path));
  assert(std::filesystem::file_size(blocks_path) > 0);
  std::cout << "blocks file size: " << std::filesystem::file_size(blocks_path) << " bytes\n";

  // Verify index file has entries
  const auto index_path = staging_dir / "embeddings" / "embeddings.index.jsonl";
  assert(std::filesystem::exists(index_path));
  assert(std::filesystem::file_size(index_path) > 0);

  // Verify sets file
  const auto sets_path = staging_dir / "embeddings" / "embedding_sets.json";
  assert(std::filesystem::exists(sets_path));
  assert(std::filesystem::file_size(sets_path) > 0);

  // Verify entry dimensions
  for (const auto& entry : result.entries) {
    assert(entry.dim == 768);
    assert(!entry.payload_blake3.empty());
    assert(!entry.block_blake3.empty());
    assert(entry.block_length > 0);
  }

  std::cout << "\nSUCCESS: Real embedding generation verified!\n";

  std::filesystem::remove_all(root);
  return 0;
}
