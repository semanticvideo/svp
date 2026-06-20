#include "svp/vision/depth_generation.hpp"

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

nlohmann::json make_depth_processor_provenance(
    const std::string& model_id,
    const std::string& model_bundle_id,
    const std::string& execution_provider,
    const std::string& status,
    const std::string& note) {
  return {
      {"id", "proc_depth_0001"},
      {"name", "svp depth generation"},
      {"version", "svp-depth-v1"},
      {"input_refs", {"canonical_analysis_raster_frames"}},
      {"output_refs", {
          "spatial/depth.index.jsonl",
          "spatial/depth.blocks.svpdz"
      }},
      {"model_refs", model_id.empty() ? nlohmann::json::array() :
          nlohmann::json::array({model_id})},
      {"task_ids", {"task.vision.depth_generation"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", status},
      {"runtime", "onnxruntime"},
      {"execution_provider", execution_provider},
      {"note", note}
  };
}

}  // namespace

DepthGenerationResult generate_depth_blocks(
    const DepthGenerationOptions& options,
    const std::filesystem::path& staging_dir) {
  DepthGenerationResult result;
  result.onnx_runtime_available = svp::models::OnnxSession::is_available();
  result.model_id = options.model_id;
  result.execution_provider = options.execution_provider;

  if (!result.onnx_runtime_available) {
    result.blocker = "ONNX Runtime is not available in this build";
    result.processor_provenance = make_depth_processor_provenance(
        "", "", options.execution_provider, "not_run", result.blocker);
    return result;
  }

  const auto cache_root = options.model_cache_root.empty()
      ? svp::models::model_cache_root()
      : options.model_cache_root;

  auto bundle_dir = find_model_bundle_dir(cache_root, options.model_id);
  if (!bundle_dir.has_value()) {
    result.blocker = "Depth model bundle not found in cache: " + options.model_id;
    result.processor_provenance = make_depth_processor_provenance(
        options.model_id, "", options.execution_provider, "not_run",
        result.blocker);
    return result;
  }

  result.depth_model_available = true;

  std::optional<svp::models::ModelBundleManifest> manifest_opt;
  try {
    manifest_opt = svp::models::load_model_bundle_manifest(
        *bundle_dir / "model.svpmodel.json");
    result.model_bundle_id = manifest_opt->model_bundle_id;
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load model manifest: ") + e.what();
    result.processor_provenance = make_depth_processor_provenance(
        options.model_id, "", options.execution_provider, "not_run",
        result.blocker);
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
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  // Real decoded/canonical video frame input is not yet wired into the
  // depth generation path. Until frame decoding is integrated, depth
  // generation remains blocked. Do not produce synthetic or fake depth.
  result.blocker = "Real decoded/canonical video frame input is not yet wired; "
      "depth generation is blocked until frame decoding is integrated";
  result.processor_provenance = make_depth_processor_provenance(
      manifest.model_id, manifest.model_bundle_id,
      options.execution_provider, "not_run", result.blocker);
  return result;
}

nlohmann::json depth_generation_result_to_json(
    const DepthGenerationResult& result) {
  nlohmann::json entries_arr = nlohmann::json::array();
  for (const auto& e : result.entries) {
    entries_arr.push_back({
        {"id", e.id},
        {"frame_id", e.frame_id},
        {"width", e.width},
        {"height", e.height},
        {"block_offset", e.block_offset},
        {"block_length", e.block_length},
        {"payload_blake3", e.payload_blake3},
        {"block_blake3", e.block_blake3}
    });
  }

  return {
      {"onnx_runtime_available", result.onnx_runtime_available},
      {"depth_model_available", result.depth_model_available},
      {"depth_generation_run", result.depth_generation_run},
      {"depth_blocks_written", result.depth_blocks_written},
      {"depth_index_written", result.depth_index_written},
      {"model_id", result.model_id},
      {"model_bundle_id", result.model_bundle_id},
      {"execution_provider", result.execution_provider},
      {"blocker", result.blocker},
      {"entries", entries_arr},
      {"processor_provenance", result.processor_provenance}
  };
}

}  // namespace svp::vision
