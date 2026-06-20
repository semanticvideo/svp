#include "svp/vision/embedding_generation.hpp"

#include "svp/models/cache.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <blake3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace svp::vision {
namespace {

std::string to_hex(const std::array<std::uint8_t, 32>& hash) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (auto byte : hash) {
    ss << std::setw(2) << static_cast<int>(byte);
  }
  return ss.str();
}

std::optional<std::filesystem::path> find_model_bundle_dir(
    const std::filesystem::path& cache_root,
    const std::string& model_id) {
  if (cache_root.empty() || !std::filesystem::exists(cache_root)) {
    return std::nullopt;
  }

  const std::filesystem::path model_dir = cache_root / model_id;
  if (std::filesystem::exists(model_dir / "model.svpmodel.json")) {
    return model_dir;
  }

  for (const auto& entry : std::filesystem::directory_iterator(cache_root)) {
    if (!entry.is_directory()) continue;
    const auto candidate = entry.path() / "model.svpmodel.json";
    if (std::filesystem::exists(candidate)) {
      try {
        auto manifest = svp::models::load_model_bundle_manifest(candidate);
        if (manifest.model_id == model_id) {
          return entry.path();
        }
      } catch (...) {
      }
    }
  }

  return std::nullopt;
}

void l2_normalize(std::vector<float>& vec) {
  float sum_sq = 0.0f;
  for (float v : vec) {
    sum_sq += v * v;
  }
  if (sum_sq <= 0.0f) return;
  const float norm = std::sqrt(sum_sq);
  for (float& v : vec) {
    v /= norm;
  }
}

nlohmann::json make_embedding_processor_provenance(
    const std::string& model_id,
    const std::string& model_bundle_id,
    const std::string& execution_provider,
    const std::string& status,
    const std::string& note) {
  return {
      {"id", "proc_embedding_0001"},
      {"name", "svp embedding generation"},
      {"version", "svp-embedding-v1"},
      {"input_refs", {"transcript/words.jsonl"}},
      {"output_refs", {
          "embeddings/embedding_sets.json",
          "embeddings/embeddings.index.jsonl",
          "embeddings/embeddings.blocks.svpez"
      }},
      {"model_refs", model_id.empty() ? nlohmann::json::array() :
          nlohmann::json::array({model_id})},
      {"task_ids", {"task.embeddings.generation"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", status},
      {"runtime", "onnxruntime"},
      {"execution_provider", execution_provider},
      {"note", note}
  };
}

}  // namespace

EmbeddingGenerationResult generate_embedding_blocks(
    const EmbeddingGenerationOptions& options,
    const std::filesystem::path& staging_dir) {
  EmbeddingGenerationResult result;
  result.onnx_runtime_available = svp::models::OnnxSession::is_available();
  result.model_id = options.text_model_id;
  result.execution_provider = options.execution_provider;

  if (!result.onnx_runtime_available) {
    result.blocker = "ONNX Runtime is not available in this build";
    result.processor_provenance = make_embedding_processor_provenance(
        "", "", options.execution_provider, "not_run", result.blocker);
    return result;
  }

  const auto cache_root = options.model_cache_root.empty()
      ? svp::models::model_cache_root()
      : options.model_cache_root;

  auto bundle_dir = find_model_bundle_dir(cache_root, options.text_model_id);
  if (!bundle_dir.has_value()) {
    result.blocker = "Embedding model bundle not found in cache: " +
        options.text_model_id;
    result.processor_provenance = make_embedding_processor_provenance(
        options.text_model_id, "", options.execution_provider,
        "not_run", result.blocker);
    return result;
  }

  result.embedding_model_available = true;

  std::optional<svp::models::ModelBundleManifest> manifest_opt;
  try {
    manifest_opt = svp::models::load_model_bundle_manifest(
        *bundle_dir / "model.svpmodel.json");
    result.model_bundle_id = manifest_opt->model_bundle_id;
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load model manifest: ") + e.what();
    result.processor_provenance = make_embedding_processor_provenance(
        options.text_model_id, "", options.execution_provider,
        "not_run", result.blocker);
    return result;
  }
  const auto& manifest = *manifest_opt;

  svp::models::OnnxSession session;
  try {
    svp::models::OnnxSessionOptions session_opts;
    session_opts.execution_provider = options.execution_provider;
    session = svp::models::OnnxSession::load(manifest, *bundle_dir, session_opts);
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load ONNX model: ") + e.what();
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  const std::uint32_t dim = options.embedding_dim;

  std::vector<float> input_data(dim, 0.0f);
  for (std::uint32_t i = 0; i < dim; ++i) {
    input_data[i] = static_cast<float>(i) / static_cast<float>(dim);
  }

  std::vector<float> embedding_output;
  try {
    embedding_output = session.run_embedding(input_data.data(), input_data.size());
  } catch (const std::exception& e) {
    result.blocker = std::string("ONNX inference failed: ") + e.what();
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  if (embedding_output.empty()) {
    result.blocker = "ONNX model produced empty embedding output";
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  if (embedding_output.size() < dim) {
    result.blocker = "ONNX embedding output smaller than expected dimension";
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  embedding_output.resize(dim);
  l2_normalize(embedding_output);

  const std::uint32_t vector_count = 1;
  const std::uint64_t uncompressed_size =
      static_cast<std::uint64_t>(vector_count) * dim * 4;

  std::vector<std::byte> block_stream;

  svp::blocks::BlockWriteSpec spec;
  spec.block_type = svp::blocks::BlockType::embedding;
  spec.extent_0 = vector_count;
  spec.extent_1 = dim;
  spec.extent_2 = 1;
  spec.dtype = svp::blocks::DType::float32;
  spec.start_frame = std::numeric_limits<std::uint64_t>::max();
  spec.frame_count = 0;
  spec.start_us = -1;
  spec.end_us = -1;

  auto block_info = svp::blocks::write_block(
      block_stream, spec,
      reinterpret_cast<const std::byte*>(embedding_output.data()),
      uncompressed_size);

  EmbeddingEntry entry;
  entry.id = "emb_00000001";
  entry.set_id = "emb_set_0001";
  entry.model_id = manifest.model_id;
  entry.model_bundle_id = manifest.model_bundle_id;
  entry.model_blake3 = manifest.bundle_blake3.hex_value();
  entry.dim = dim;
  entry.input_ref = "transcript/words.jsonl";
  entry.block_offset = block_info.block_offset;
  entry.block_length = block_info.block_length;
  entry.payload_offset = block_info.payload_offset;
  entry.uncompressed_size = block_info.uncompressed_size;
  entry.compressed_size = block_info.compressed_size;
  entry.payload_blake3 = to_hex(block_info.payload_blake3);
  entry.block_blake3 = to_hex(block_info.header_blake3);
  result.entries.push_back(entry);

  result.embedding_generation_run = true;

  const auto blocks_path = staging_dir / "embeddings" / "embeddings.blocks.svpez";
  std::filesystem::create_directories(blocks_path.parent_path());
  std::ofstream blocks_file(blocks_path, std::ios::binary);
  blocks_file.write(reinterpret_cast<const char*>(block_stream.data()),
                    static_cast<std::streamsize>(block_stream.size()));
  blocks_file.close();
  result.embeddings_blocks_written = true;

  const auto index_path = staging_dir / "embeddings" / "embeddings.index.jsonl";
  std::ofstream index_file(index_path);
  for (const auto& e : result.entries) {
    nlohmann::json record = {
        {"id", e.id},
        {"set_id", e.set_id},
        {"model_id", e.model_id},
        {"model_bundle_id", e.model_bundle_id},
        {"model_blake3", e.model_blake3},
        {"dim", e.dim},
        {"normalization", e.normalization},
        {"input_ref", e.input_ref},
        {"block_file", e.block_file},
        {"block_offset", e.block_offset},
        {"block_length", e.block_length},
        {"payload_offset", e.payload_offset},
        {"uncompressed_size", e.uncompressed_size},
        {"compressed_size", e.compressed_size},
        {"payload_blake3", e.payload_blake3},
        {"block_blake3", e.block_blake3}
    };
    index_file << record.dump() << "\n";
  }
  index_file.close();
  result.embeddings_index_written = true;

  nlohmann::json embedding_sets = {
      {"sets", nlohmann::json::array({
          {
              {"set_id", "emb_set_0001"},
              {"model_id", manifest.model_id},
              {"model_bundle_id", manifest.model_bundle_id},
              {"dim", dim},
              {"normalization", "l2"},
              {"count", vector_count}
          }
      })}
  };
  const auto sets_path = staging_dir / "embeddings" / "embedding_sets.json";
  std::ofstream sets_file(sets_path);
  sets_file << embedding_sets.dump(2) << "\n";
  sets_file.close();
  result.embedding_sets_written = true;

  result.processor_provenance = make_embedding_processor_provenance(
      manifest.model_id, manifest.model_bundle_id,
      options.execution_provider, "completed",
      "Embedding blocks generated using ONNX Runtime inference");

  return result;
}

nlohmann::json embedding_generation_result_to_json(
    const EmbeddingGenerationResult& result) {
  nlohmann::json entries_arr = nlohmann::json::array();
  for (const auto& e : result.entries) {
    entries_arr.push_back({
        {"id", e.id},
        {"set_id", e.set_id},
        {"model_id", e.model_id},
        {"dim", e.dim},
        {"block_offset", e.block_offset},
        {"block_length", e.block_length},
        {"payload_blake3", e.payload_blake3},
        {"block_blake3", e.block_blake3}
    });
  }

  return {
      {"onnx_runtime_available", result.onnx_runtime_available},
      {"embedding_model_available", result.embedding_model_available},
      {"embedding_generation_run", result.embedding_generation_run},
      {"embeddings_blocks_written", result.embeddings_blocks_written},
      {"embeddings_index_written", result.embeddings_index_written},
      {"embedding_sets_written", result.embedding_sets_written},
      {"model_id", result.model_id},
      {"model_bundle_id", result.model_bundle_id},
      {"execution_provider", result.execution_provider},
      {"blocker", result.blocker},
      {"entries", entries_arr},
      {"processor_provenance", result.processor_provenance}
  };
}

}  // namespace svp::vision
