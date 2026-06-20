#include "svp/vision/embedding_generation.hpp"

#include "svp/models/cache.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/models/verification.hpp"

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

  // BLAKE3 verification of model bundle files before any ONNX execution
  auto verify_report = svp::models::verify_manifest_files(manifest, *bundle_dir);
  if (!verify_report.ok()) {
    std::string verify_errors;
    for (const auto& issue : verify_report.issues) {
      if (issue.severity == svp::models::VerificationSeverity::error) {
        verify_errors += issue.message + "; ";
      }
    }
    result.blocker = "Model bundle BLAKE3 verification failed: " + verify_errors;
    result.processor_provenance = make_embedding_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  // Real source-derived embedding input (transcript text, vision features,
  // etc.) is not yet wired into the embedding generation path. Until real
  // package content is available as embedding input, generation remains
  // blocked. Do not produce synthetic or fake embedding vectors.
  result.blocker = "Real source-derived embedding input is not yet wired; "
      "embedding generation is blocked until transcript/text or other "
      "defined embedding input is integrated";
  result.processor_provenance = make_embedding_processor_provenance(
      manifest.model_id, manifest.model_bundle_id,
      options.execution_provider, "not_run", result.blocker);
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
