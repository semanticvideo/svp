#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct SpatialEmbeddingPlaceholderSummary {
  std::size_t depth_index_written = false;
  std::size_t depth_blocks_written = false;
  std::size_t masks_index_written = false;
  std::size_t masks_blocks_written = false;
  std::size_t embedding_sets_written = false;
  std::size_t embeddings_index_written = false;
  std::size_t embeddings_blocks_written = false;
  std::size_t provenance_records_added = 0;
};

/**
 * Writes honest not-run/placeholder files for spatial and embedding
 * package sections.
 *
 * - spatial/depth.index.jsonl: empty (no depth observations produced)
 * - spatial/depth.blocks.svpdz: zero-length placeholder (validator requires
 *   real depth blocks with frame ranges; this file exists so the package
 *   entry is present, but the validator will report it as invalid until
 *   real depth estimation is run)
 * - spatial/masks.index.jsonl: empty (no mask observations)
 * - spatial/masks.blocks.svpmz: zero-length valid empty mask block stream
 * - embeddings/embedding_sets.json: {"sets": []} (no embedding sets generated)
 * - embeddings/embeddings.index.jsonl: empty (no embedding index entries)
 * - embeddings/embeddings.blocks.svpez: zero-length placeholder (validator
 *   requires real embedding blocks with model output; this file exists so
 *   the package entry is present, but the validator will report it as
 *   invalid until real model inference is run)
 *
 * Also appends honest provenance processor records for the placeholder
 * artifacts to provenance/processors.jsonl.
 */
[[nodiscard]] SpatialEmbeddingPlaceholderSummary write_spatial_and_embedding_placeholders(
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json spatial_embedding_placeholder_summary_to_json(
    const SpatialEmbeddingPlaceholderSummary& summary);

}  // namespace svp::package
