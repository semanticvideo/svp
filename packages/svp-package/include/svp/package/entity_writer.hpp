#pragma once

#include "svp/vision/mask_writer.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct EntityWriteSummary {
  bool entities_written = false;
  bool tracks_written = false;
  bool regions_written = false;
  bool masks_written = false;
  std::size_t entity_count = 0;
  std::size_t track_count = 0;
  std::size_t region_count = 0;
  std::size_t mask_count = 0;
  std::size_t processors_written = 0;
  std::size_t duplicate_processors_merged = 0;
  std::size_t skipped_missing_evidence = 0;
};

/**
 * Writes entity and entity-track artifacts to staging:
 * - entities/entities.jsonl
 * - entities/entity_tracks.jsonl
 *
 * When visual entity tracker output is already present in entities/entities.jsonl,
 * this function preserves those visual-owned entities and does NOT create
 * duplicate text-derived persistent entities. OCR text regions remain as text
 * observations and evidence, not as duplicate persistent entities.
 *
 * When no visual entities exist (fallback mode), entities are derived from
 * OCR text region evidence with honest provenance noting the fallback status.
 * Text regions that share the same normalized text content are grouped into
 * a single entity with a single track spanning all observations.
 *
 * Also appends a processor record for the entity generator to
 * provenance/processors.jsonl.
 */
[[nodiscard]] EntityWriteSummary write_entity_artifacts(
    const std::filesystem::path& staging_dir);

/**
 * Writes visual entity, track, region, and mask artifacts to staging:
 * - entities/entities.jsonl (overwritten with visual-owned entities)
 * - entities/entity_tracks.jsonl (overwritten with visual-owned tracks)
 * - spatial/regions.jsonl
 * - spatial/masks.index.jsonl + spatial/masks.blocks.svpmz
 *
 * Evidence source: EntityTrackResult from the visual entity tracker (§20.6).
 * Also writes processor provenance for the visual entity tracker.
 */
[[nodiscard]] EntityWriteSummary write_visual_entity_artifacts(
    const std::filesystem::path& staging_dir,
    const svp::vision::EntityTrackResult& tracker_result,
    const std::vector<svp::vision::MaskWriteEntry>* preencoded_masks = nullptr);

[[nodiscard]] nlohmann::json entity_write_summary_to_json(
    const EntityWriteSummary& summary);

}  // namespace svp::package
