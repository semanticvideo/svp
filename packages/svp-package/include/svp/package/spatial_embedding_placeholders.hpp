#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::media { struct MediaIngestPlan; }

namespace svp::package {

struct SpatialEmbeddingPlaceholderSummary {
  std::size_t depth_index_written = false;
  std::size_t depth_blocks_written = false;
  std::size_t depth_placeholder_written = false;
  std::size_t depth_generation_run = false;
  std::size_t masks_index_written = false;
  std::size_t masks_blocks_written = false;
  std::size_t embedding_sets_written = false;
  std::size_t embeddings_index_written = false;
  std::size_t embeddings_blocks_written = false;
  std::size_t embedding_generation_run = false;
  std::size_t model_runtime_available = false;
  std::size_t depth_model_available = false;
  std::size_t depth_model_verified = false;
  std::size_t depth_frame_input_available = false;
  std::size_t embedding_model_available = false;
  std::size_t ocr_available = false;
  std::size_t ocr_frame_input_available = false;
  std::size_t ocr_detection_run = false;
  std::size_t ocr_recognition_run = false;
  std::size_t text_observation_count = 0;
  std::size_t numeric_value_count = 0;
  std::size_t provenance_records_added = 0;
  nlohmann::json depth_generation_detail;
  nlohmann::json embedding_generation_detail;
  nlohmann::json ocr_generation_detail;
};

/**
 * Writes spatial and embedding package sections.
 *
 * When ONNX Runtime is available and model bundles are found in the
 * SVP model cache, real depth and embedding block streams are produced
 * using the svp-vision generation modules.
 *
 * When models or runtime are unavailable, honest placeholder files are
 * written instead:
 * - spatial/depth.index.jsonl: empty (no depth observations produced)
 * - spatial/depth.blocks.svpdz: zero-length placeholder
 * - spatial/masks.index.jsonl: empty (no mask observations)
 * - spatial/masks.blocks.svpmz: zero-length valid empty mask block stream
 * - embeddings/embedding_sets.json: {"sets": []} (no embedding sets generated)
 * - embeddings/embeddings.index.jsonl: empty (no embedding index entries)
 * - embeddings/embeddings.blocks.svpez: zero-length placeholder
 *
 * Also appends honest provenance processor records to
 * provenance/processors.jsonl.
 *
 * When media_plan and ffmpeg_path are provided, real canonical frames are
 * decoded and passed to depth generation for ONNX inference.
 */
[[nodiscard]] SpatialEmbeddingPlaceholderSummary write_spatial_and_embedding_placeholders(
    const std::filesystem::path& staging_dir,
    bool model_runtime_available,
    const nlohmann::json& media_plan_json = {},
    const std::filesystem::path& model_cache_root = {},
    const svp::media::MediaIngestPlan* media_plan = nullptr,
    const std::filesystem::path& ffmpeg_path = {},
    const std::filesystem::path& tesseract_path = {});

[[nodiscard]] nlohmann::json spatial_embedding_placeholder_summary_to_json(
    const SpatialEmbeddingPlaceholderSummary& summary);

}  // namespace svp::package
