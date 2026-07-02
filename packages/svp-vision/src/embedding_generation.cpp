#include "svp/vision/embedding_generation.hpp"

#include "svp/core/memory_diagnostics.hpp"
#include "svp/models/cache.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/models/verification.hpp"
#include "svp/vision/text_tokenizer.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace svp::vision {
namespace {

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
      {"input_refs", nlohmann::json::array()},
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

std::string hash_to_hex(const std::array<std::uint8_t, 32>& hash) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < hash.size(); ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(hash[i]);
  }
  return oss.str();
}

struct TextObservationInput {
  std::string id;
  std::string text;
  std::string input_ref;
  std::string input_kind;
  std::int64_t start_us = -1;
  std::int64_t end_us = -1;
};

std::vector<TextObservationInput> load_text_observations_from_staging(
    const std::filesystem::path& staging_dir) {
  std::vector<TextObservationInput> inputs;
  const std::filesystem::path text_obs_path =
      staging_dir / "text" / "text_observations.jsonl";
  if (!std::filesystem::exists(text_obs_path)) {
    return inputs;
  }

  std::ifstream input(text_obs_path);
  if (!input) {
    return inputs;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    try {
      nlohmann::json record = nlohmann::json::parse(line);
      TextObservationInput obs;
      obs.id = record.value("text_observation_id", "");
      obs.text = record.value("normalized_text", "");
      if (obs.text.empty()) {
        obs.text = record.value("raw_text", "");
      }
      obs.input_ref = record.value("text_region_id", "");
      obs.input_kind = "text_observation";
      if (obs.text.empty() || obs.id.empty()) {
        continue;
      }
      inputs.push_back(std::move(obs));
    } catch (...) {
    }
  }
  return inputs;
}

std::vector<float> mean_pool_and_normalize(
    const std::vector<float>& last_hidden_state,
    const std::vector<std::int64_t>& attention_mask,
    std::size_t seq_len,
    std::uint32_t embedding_dim) {
  std::vector<float> pooled(embedding_dim, 0.0f);
  float mask_sum = 0.0f;

  for (std::size_t t = 0; t < seq_len; ++t) {
    if (attention_mask[t] == 0) continue;
    mask_sum += 1.0f;
    for (std::uint32_t d = 0; d < embedding_dim; ++d) {
      pooled[d] += last_hidden_state[t * embedding_dim + d];
    }
  }

  if (mask_sum > 0.0f) {
    const float inv = 1.0f / mask_sum;
    for (std::uint32_t d = 0; d < embedding_dim; ++d) {
      pooled[d] *= inv;
    }
  }

  float norm = 0.0f;
  for (std::uint32_t d = 0; d < embedding_dim; ++d) {
    norm += pooled[d] * pooled[d];
  }
  norm = std::sqrt(norm);
  if (norm > 1e-12f) {
    const float inv_norm = 1.0f / norm;
    for (std::uint32_t d = 0; d < embedding_dim; ++d) {
      pooled[d] *= inv_norm;
    }
  }

  return pooled;
}

bool validate_embedding(const std::vector<float>& embedding,
                        std::uint32_t expected_dim) {
  if (embedding.size() != expected_dim) return false;
  bool has_nan = false;
  bool has_inf = false;
  float norm_sq = 0.0f;
  for (std::uint32_t d = 0; d < expected_dim; ++d) {
    if (std::isnan(embedding[d])) has_nan = true;
    if (std::isinf(embedding[d])) has_inf = true;
    norm_sq += embedding[d] * embedding[d];
  }
  if (has_nan || has_inf) return false;
  const float norm = std::sqrt(norm_sq);
  if (norm < 0.9f || norm > 1.1f) return false;
  if (norm_sq < 1e-20f) return false;
  return true;
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

  auto verify_report = svp::models::verify_manifest_files(manifest, *bundle_dir);
  if (!verify_report.ok()) {
    std::string verify_errors;
    for (const auto& issue : verify_report.issues) {
      if (issue.severity == svp::models::VerificationSeverity::error) {
        verify_errors += issue.message + "; ";
      }
    }
    result.blocker = "Manifest file BLAKE3 hash verification failed: " + verify_errors;
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  auto text_inputs = load_text_observations_from_staging(staging_dir);
  svp::core::check_memory_limit("text_embedding.inputs_loaded", {
      {"text_input_count", std::to_string(text_inputs.size())}
  });
  if (text_inputs.empty()) {
    result.blocker = "No real source-derived text observations found in "
        "staging (text/text_observations.jsonl); embedding generation is "
        "blocked until OCR text or other defined embedding input is available";
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  WordPieceTokenizer tokenizer;
  std::filesystem::path vocab_path;
  for (const auto& file : manifest.files) {
    if (file.role == "tokenizer_vocab") {
      vocab_path = *bundle_dir / file.path;
      break;
    }
  }
  if (vocab_path.empty()) {
    result.blocker = "Tokenizer vocabulary file not found in model bundle manifest";
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }
  if (!tokenizer.load(vocab_path)) {
    result.blocker = "Failed to load tokenizer vocabulary: " + vocab_path.string();
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  svp::models::OnnxSession session;
  try {
    svp::models::OnnxSessionOptions session_opts;
    session_opts.execution_provider = options.execution_provider;
    session = svp::models::OnnxSession::load(manifest, *bundle_dir, session_opts);
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load ONNX session: ") + e.what();
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  std::string model_blake3 = manifest.bundle_blake3.hex_value();

  const std::filesystem::path emb_blocks_path =
      staging_dir / "embeddings" / "embeddings.blocks.svpez";
  const std::filesystem::path emb_blocks_tmp_path =
      staging_dir / "embeddings" / "embeddings.blocks.svpez.tmp";
  const std::filesystem::path emb_index_path =
      staging_dir / "embeddings" / "embeddings.index.jsonl";
  const std::filesystem::path emb_sets_path =
      staging_dir / "embeddings" / "embedding_sets.json";
  std::filesystem::create_directories(emb_blocks_path.parent_path());

  std::ofstream block_out(emb_blocks_tmp_path, std::ios::binary);
  if (!block_out) {
    result.blocker = "Failed to open embedding blocks temp file: " +
        emb_blocks_tmp_path.string();
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "error", result.blocker);
    return result;
  }

  std::vector<nlohmann::json> index_entries;

  const std::size_t total_text_inputs = text_inputs.size();
  if (options.on_progress) {
    options.on_progress(0, total_text_inputs);
  }

  for (std::size_t input_index = 0; input_index < text_inputs.size(); ++input_index) {
    const auto& text_input = text_inputs[input_index];
    if (input_index == 0 || ((input_index + 1) % 25) == 0 ||
        input_index + 1 == text_inputs.size()) {
      svp::core::check_memory_limit("text_embedding.item.begin", {
          {"index", std::to_string(input_index)},
          {"total", std::to_string(text_inputs.size())},
          {"text_id", text_input.id},
          {"text_size", std::to_string(text_input.text.size())}
      });
    }
    TokenizedText tokenized = tokenizer.tokenize(text_input.text, 512);

    svp::models::TextEmbeddingOutput embedding_output;
    try {
      embedding_output = session.run_text_embedding(
          tokenized.input_ids.data(),
          tokenized.token_type_ids.data(),
          tokenized.attention_mask.data(),
          1,
          tokenized.seq_len);
    } catch (const std::exception& e) {
      result.blocker = std::string("ONNX text embedding inference failed for ") +
          text_input.id + ": " + e.what();
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    if (embedding_output.shape.size() != 3) {
      result.blocker = std::string("ONNX output shape validation failed for ") +
          text_input.id + ": expected rank-3 output, got rank " +
          std::to_string(embedding_output.shape.size());
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    for (std::size_t i = 0; i < 3; ++i) {
      if (embedding_output.shape[i] <= 0) {
        result.blocker = std::string("ONNX output shape validation failed for ") +
            text_input.id + ": dimension " + std::to_string(i) +
            " is not positive, got " +
            std::to_string(embedding_output.shape[i]);
        result.processor_provenance = make_embedding_processor_provenance(
            manifest.model_id, manifest.model_bundle_id,
            options.execution_provider, "error", result.blocker);
        return result;
      }
    }

    if (embedding_output.shape[0] != 1) {
      result.blocker = std::string("ONNX output batch mismatch for ") +
          text_input.id + ": expected 1, got " +
          std::to_string(embedding_output.shape[0]);
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    const std::size_t out_batch =
        static_cast<std::size_t>(embedding_output.shape[0]);
    const std::size_t out_seq_len =
        static_cast<std::size_t>(embedding_output.shape[1]);
    const std::size_t out_dim =
        static_cast<std::size_t>(embedding_output.shape[2]);

    const std::size_t expected_elements = out_batch * out_seq_len * out_dim;
    if (embedding_output.data.size() != expected_elements) {
      result.blocker = std::string("ONNX output element count mismatch for ") +
          text_input.id + ": shape implies " +
          std::to_string(expected_elements) + " elements, got " +
          std::to_string(embedding_output.data.size());
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    if (out_dim != options.embedding_dim) {
      result.blocker = std::string("ONNX output dimension mismatch for ") +
          text_input.id + ": expected " +
          std::to_string(options.embedding_dim) + ", got " +
          std::to_string(out_dim);
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    if (out_seq_len != tokenized.seq_len) {
      result.blocker = std::string("ONNX output seq_len mismatch for ") +
          text_input.id + ": expected " +
          std::to_string(tokenized.seq_len) + ", got " +
          std::to_string(out_seq_len);
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    std::vector<float> embedding = mean_pool_and_normalize(
        embedding_output.data, tokenized.attention_mask,
        tokenized.seq_len, options.embedding_dim);

    if (!validate_embedding(embedding, options.embedding_dim)) {
      result.blocker = std::string("Embedding validation failed for ") +
          text_input.id + ": invalid vector (NaN, Inf, zero, or wrong dimension)";
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    svp::blocks::BlockWriteSpec spec;
    spec.block_type = svp::blocks::BlockType::embedding;
    spec.extent_0 = 1;
    spec.extent_1 = options.embedding_dim;
    spec.extent_2 = 1;
    spec.dtype = svp::blocks::DType::float32;
    spec.start_frame = std::numeric_limits<std::uint64_t>::max();
    spec.frame_count = 0;
    spec.start_us = -1;
    spec.end_us = -1;

    svp::blocks::WrittenBlockInfo block_info;
    try {
      block_info = svp::blocks::write_block_to_stream(
          block_out, spec,
          reinterpret_cast<const std::byte*>(embedding.data()),
          embedding.size() * sizeof(float));
    } catch (const std::exception& e) {
      result.blocker = std::string("Failed to write embedding block for ") +
          text_input.id + ": " + e.what();
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    const std::string payload_hex = hash_to_hex(block_info.payload_blake3);
    const std::string block_hex = hash_to_hex(block_info.header_blake3);

    EmbeddingEntry entry;
    entry.id = "embed_" + text_input.id;
    entry.set_id = "embedset_text_nomic_v15";
    entry.model_id = manifest.model_id;
    entry.model_bundle_id = manifest.model_bundle_id;
    entry.model_blake3 = model_blake3;
    entry.dim = options.embedding_dim;
    entry.normalization = "l2";
    entry.input_ref = text_input.input_ref;
    entry.block_file = "embeddings/embeddings.blocks.svpez";
    entry.block_offset = block_info.block_offset;
    entry.block_length = block_info.block_length;
    entry.payload_offset = block_info.payload_offset;
    entry.uncompressed_size = block_info.uncompressed_size;
    entry.compressed_size = block_info.compressed_size;
    entry.payload_blake3 = payload_hex;
    entry.block_blake3 = block_hex;
    const std::size_t vector_index = result.entries.size();
    result.entries.push_back(entry);

    index_entries.push_back({
        {"id", entry.id},
        {"embedding_set_id", entry.set_id},
        {"input_ref", entry.input_ref},
        {"input_kind", text_input.input_kind},
        {"block_file", entry.block_file},
        {"block_offset", entry.block_offset},
        {"block_length", entry.block_length},
        {"payload_offset", entry.payload_offset},
        {"uncompressed_size", entry.uncompressed_size},
        {"compressed_size", entry.compressed_size},
        {"vector_index", vector_index},
        {"dimension", entry.dim},
        {"dtype", "float32"},
        {"payload_blake3", entry.payload_blake3},
        {"block_blake3", entry.block_blake3}
    });

    if (options.on_progress) {
      options.on_progress(result.entries.size(), total_text_inputs);
    }
    if (input_index == 0 || ((input_index + 1) % 25) == 0 ||
        input_index + 1 == text_inputs.size()) {
      svp::core::check_memory_limit("text_embedding.item.complete", {
          {"index", std::to_string(input_index)},
          {"total", std::to_string(text_inputs.size())},
          {"entries", std::to_string(result.entries.size())},
          {"index_entries", std::to_string(index_entries.size())}
      });
    }
  }

  if (result.entries.empty()) {
    result.blocker = "No valid embeddings were generated from text observations";
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  block_out.close();
  if (!block_out) {
    result.blocker = "Failed to finalize embedding blocks file: " +
        emb_blocks_tmp_path.string();
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "error", result.blocker);
    return result;
  }
  std::filesystem::remove(emb_blocks_path);
  std::filesystem::rename(emb_blocks_tmp_path, emb_blocks_path);
  result.embeddings_blocks_written = true;

  {
    std::ofstream out(emb_index_path);
    if (!out) {
      result.blocker = "Failed to open embedding index file: " + emb_index_path.string();
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }
    for (const nlohmann::json& entry : index_entries) {
      out << entry.dump() << "\n";
    }
  }
  result.embeddings_index_written = true;

  {
    nlohmann::json sets_json = {
        {"sets", nlohmann::json::array({
            {
                {"id", "embedset_text_nomic_v15"},
                {"modality", "text"},
                {"model_id", manifest.model_id},
                {"model_blake3", model_blake3},
                {"dimension", options.embedding_dim},
                {"dtype", "float32"},
                {"normalized", true},
                {"source_slug", "nomic-ai/nomic-embed-text-v1.5"}
            }
        })}
    };
    std::ofstream out(emb_sets_path);
    if (!out) {
      result.blocker = "Failed to open embedding sets file: " + emb_sets_path.string();
      result.processor_provenance = make_embedding_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }
    out << sets_json.dump(2) << "\n";
  }
  result.embedding_sets_written = true;
  result.embedding_generation_run = true;

  result.processor_provenance = make_embedding_processor_provenance(
      manifest.model_id, manifest.model_bundle_id,
      options.execution_provider, "completed",
      "Text embeddings generated from " + std::to_string(result.entries.size()) +
      " source-derived text observations using ONNX Runtime; "
      "mean pooling with attention mask and L2 normalization applied");

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
