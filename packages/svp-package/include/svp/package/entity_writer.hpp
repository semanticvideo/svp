#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct EntityWriteSummary {
  bool entities_written = false;
  bool tracks_written = false;
  std::size_t entity_count = 0;
  std::size_t track_count = 0;
  std::size_t processors_written = 0;
  std::size_t duplicate_processors_merged = 0;
  std::size_t skipped_missing_evidence = 0;
};

/**
 * Writes honest, source-derived entity and entity-track artifacts to staging:
 * - entities/entities.jsonl
 * - entities/entity_tracks.jsonl
 *
 * Evidence source: OCR text regions from text/text_regions.jsonl, linked to
 * text observations from text/text_observations.jsonl. Text regions that share
 * the same normalized text content are grouped into a single entity with a
 * single track spanning all observations. This is conservative: we do not
 * claim robust cross-frame tracking. Each track represents observations of the
 * same visible text element across time.
 *
 * When text regions or observations are absent, valid empty files are written.
 *
 * Also appends a processor record for the entity generator to
 * provenance/processors.jsonl.
 */
[[nodiscard]] EntityWriteSummary write_entity_artifacts(
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json entity_write_summary_to_json(
    const EntityWriteSummary& summary);

}  // namespace svp::package
