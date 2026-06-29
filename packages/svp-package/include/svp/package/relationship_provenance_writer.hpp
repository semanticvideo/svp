#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct RelationshipTypeCounts {
  std::size_t text_region_shot = 0;
  std::size_t text_region_scene = 0;
  std::size_t text_observation_region = 0;
  std::size_t text_observation_evidence_crop = 0;
  std::size_t numeric_value_observation = 0;
  std::size_t word_speaker = 0;
  std::size_t word_speaker_segment = 0;
  std::size_t word_speaker_segment_unmatched = 0;
  std::size_t color_observation_target = 0;
  std::size_t depth_frame = 0;
  std::size_t embedding_source = 0;
  std::size_t text_region_overlaps_entity = 0;
  std::size_t skipped_dangling = 0;
  std::size_t semantic_visible_during_speech = 0;
  std::size_t semantic_visible_during_word_range = 0;
  std::size_t semantic_speaker_active_during_entity_visible = 0;
  std::size_t semantic_entity_appears_in_shot = 0;
  std::size_t semantic_entity_appears_in_scene = 0;
};

struct RelationshipProvenanceWriteSummary {
  std::size_t relationships_written = 0;
  std::size_t processors_written = 0;
  std::size_t duplicate_processors_merged = 0;
  RelationshipTypeCounts type_counts;
};

[[nodiscard]] RelationshipProvenanceWriteSummary write_relationships_and_provenance(
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json relationship_provenance_write_summary_to_json(
    const RelationshipProvenanceWriteSummary& summary);

}  // namespace svp::package
