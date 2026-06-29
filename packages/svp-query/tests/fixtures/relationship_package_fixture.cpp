#include "relationship_package_fixture.hpp"
#include "../query_test_io.hpp"

#include "svp/package/package_writer.hpp"

#include <cassert>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

std::filesystem::path create_test_package_with_relationships() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-rel-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_rel.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_query_rel_test_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  write_json(staging / "text" / "text_absence.json", {{"reason", "test"}});
  write_json(staging / "colors" / "color_summary.json", {{"schema_version", "svp-color-summary-v1"}});
  write_json(staging / "colors" / "color_absence.json", {{"reason", "test"}});
  write_json(staging / "embeddings" / "embedding_sets.json", {{"schema_version", "svp-embedding-sets-v1"}});
  write_file(staging / "embeddings" / "embeddings.index.jsonl", "");
  write_json(staging / "index" / "index_manifest.json", {
      {"index_schema_version", "svp-index-v1"},
      {"sqlite_file", "index.sqlite"},
      {"logical_row_stream_version", "1"},
      {"table_count", 6}, {"row_count", 10}
  });
  write_json(staging / "provenance" / "build.json", {{"build_id", "test_build"}});
  write_jsonl(staging / "provenance" / "processors.jsonl", {});
  write_jsonl(staging / "provenance" / "input_hashes.jsonl", {});
  write_jsonl(staging / "provenance" / "model_hashes.jsonl", {});
  write_json(staging / "provenance" / "validation.json", {
      {"status", "valid"},
      {"core_status", "valid"},
      {"authenticity_status", "valid"}
  });
  std::filesystem::create_directories(staging / "media" / "original");
  std::filesystem::create_directories(staging / "media" / "audio");

  // Write relationships with mixed support, semantic, and unknown types
  std::vector<nlohmann::json> relationships = {
      {{"id", "rel_001"}, {"type", "word_spoken_by"}, {"source_id", "word_001"},
       {"target_id", "speaker_001"}, {"start_us", 0}, {"end_us", 1000000},
       {"evidence", {{"source_path", "transcript/words.jsonl"}}},
       {"confidence", 0.95}, {"processor_id", "proc_test"}},
      {{"id", "rel_002"}, {"type", "appears_in_shot"}, {"source_id", "entity_001"},
       {"target_id", "shot_001"}, {"start_us", 0}, {"end_us", 5000000},
       {"evidence", {{"source_path", "entities/entities.jsonl"}}},
       {"confidence", 0.88}, {"processor_id", "proc_test"}},
      {{"id", "rel_003"}, {"type", "depth_for_frame"}, {"source_id", "depth_001"},
       {"target_id", "frame_001"}, {"start_us", 0}, {"end_us", 0},
       {"evidence", {{"source_path", "spatial/depth.index.jsonl"}}},
       {"confidence", 1.0}, {"processor_id", "proc_test"}},
      {{"id", "rel_004"}, {"type", "overlaps"}, {"source_id", "entity_001"},
       {"target_id", "entity_002"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"evidence", {{"region_ids", {"region_001", "region_002"}}, {"iou", 0.17}}},
       {"confidence", 0.79}, {"processor_id", "proc_test"}},
      {{"id", "rel_005"}, {"type", "totally_unknown_type"}, {"source_id", "entity_001"},
       {"target_id", "entity_002"}, {"start_us", 0}, {"end_us", 1000000},
       {"evidence", {{"source_path", "test"}}},
       {"confidence", 0.5}, {"processor_id", "proc_test"}},
  };
  write_jsonl(staging / "relationships" / "relationships.jsonl", relationships);

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}
