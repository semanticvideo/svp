#pragma once

#include "svp/models/reference_processor_model_ids.hpp"

#include "svp/blocks/block_writer.hpp"
#include "svp/models/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

struct EmbeddingGenerationOptions {
  std::filesystem::path model_cache_root;
  std::string text_model_id = svp::models::kNomicEmbedTextV15ModelId;
  std::string vision_model_id = svp::models::kNomicEmbedVisionV15ModelId;
  std::string execution_provider = "cpu";
  std::uint32_t embedding_dim = 768;
  std::function<void(std::size_t current, std::size_t total)> on_progress;
};

struct EmbeddingEntry {
  std::string id;
  std::string set_id;
  std::string model_id;
  std::string model_bundle_id;
  std::string model_blake3;
  std::uint32_t dim = 0;
  std::string normalization = "l2";
  std::string input_ref;
  std::string block_file = "embeddings/embeddings.blocks.svpez";
  std::uint64_t block_offset = 0;
  std::uint64_t block_length = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t compressed_size = 0;
  std::string payload_blake3;
  std::string block_blake3;
};

struct EmbeddingGenerationResult {
  bool onnx_runtime_available = false;
  bool embedding_model_available = false;
  bool embedding_generation_run = false;
  bool embeddings_blocks_written = false;
  bool embeddings_index_written = false;
  bool embedding_sets_written = false;
  std::string model_id;
  std::string model_bundle_id;
  std::string execution_provider;
  std::string blocker;
  std::vector<EmbeddingEntry> entries;
  nlohmann::json processor_provenance;
};

[[nodiscard]] EmbeddingGenerationResult generate_embedding_blocks(
    const EmbeddingGenerationOptions& options,
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json embedding_generation_result_to_json(
    const EmbeddingGenerationResult& result);

}  // namespace svp::vision
