#include "spatial_pair_fixture.hpp"
#include "../query_test_io.hpp"

#include "svp/package/package_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

std::filesystem::path create_spatial_pair_test_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-spatial-pair-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_spatial_pair.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_spatial_pair_test_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  nlohmann::json transcript_json = {
      {"language", {{"primary", "en"}, {"detected", {"en"}}, {"mode", "single"}, {"confidence", 0.94}}},
      {"duration_us", 30000000},
      {"word_count", 1},
      {"speaker_count", 1},
      {"source_audio_id", "astream_analysis_0001"},
      {"processor_id", "proc_whispercpp_0001"}
  };
  write_json(staging / "transcript" / "transcript.json", transcript_json);

  write_jsonl(staging / "transcript" / "words.jsonl", {
      {{"id", "word_000001"}, {"text", "hello"}, {"normalized_text", "hello"},
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_000001"}}
  });
  write_jsonl(staging / "transcript" / "speakers.jsonl", {
      {{"id", "speaker_0001"}, {"display_name", "Speaker 1"},
       {"total_speech_us", 500000}, {"confidence", 0.91},
       {"processor_id", "proc_whispercpp_0001"}}
  });
  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", {
      {{"segment_id", "seg_001"}, {"speaker_id", "speaker_0001"},
       {"start_us", 0}, {"end_us", 500000}}
  });
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});

  write_jsonl(staging / "timeline" / "frames.jsonl", {
      {{"id", "frame_000001"}, {"pts_us", 0}, {"shot_id", "shot_000001"}},
      {{"id", "frame_000002"}, {"pts_us", 100000}, {"shot_id", "shot_000001"}},
      {{"id", "frame_000003"}, {"pts_us", 200000}, {"shot_id", "shot_000001"}}
  });
  write_jsonl(staging / "timeline" / "shots.jsonl", {
      {{"id", "shot_000001"}, {"start_us", 0}, {"end_us", 15000000}}
  });
  write_jsonl(staging / "timeline" / "scenes.jsonl", {
      {{"id", "scene_000001"}, {"start_us", 0}, {"end_us", 30000000},
       {"shot_ids", {"shot_000001"}}}
  });

  write_jsonl(staging / "entities" / "entities.jsonl", {
      {{"id", "entity_001"}, {"entity_type", "person"}, {"label", "Person 1"}},
      {{"id", "entity_002"}, {"entity_type", "person"}, {"label", "Person 2"}}
  });
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  // Spatial regions with bounding boxes on the same frame
  // region_001 and region_002 overlap on frame_000001
  // region_003 is near region_001 on frame_000001
  // region_004 contains region_005 on frame_000002
  // entity_001 appears on frames 1-3, entity_002 on frames 1-2
  write_jsonl(staging / "spatial" / "regions.jsonl", {
      {{"id", "region_000001"}, {"entity_id", "entity_001"},
       {"frame_id", "frame_000001"}, {"pts_us", 0},
       {"box_norm", {0.1, 0.1, 0.5, 0.5}}},
      {{"id", "region_000002"}, {"entity_id", "entity_002"},
       {"frame_id", "frame_000001"}, {"pts_us", 0},
       {"box_norm", {0.3, 0.3, 0.7, 0.7}}},
      {{"id", "region_000003"}, {"entity_id", "entity_002"},
       {"frame_id", "frame_000001"}, {"pts_us", 0},
       {"box_norm", {0.6, 0.6, 0.65, 0.65}}},
      {{"id", "region_000004"}, {"entity_id", "entity_001"},
       {"frame_id", "frame_000002"}, {"pts_us", 100000},
       {"box_norm", {0.0, 0.0, 1.0, 1.0}}},
      {{"id", "region_000005"}, {"entity_id", "entity_002"},
       {"frame_id", "frame_000002"}, {"pts_us", 100000},
       {"box_norm", {0.2, 0.2, 0.4, 0.4}}},
      {{"id", "region_000006"}, {"entity_id", "entity_001"},
       {"frame_id", "frame_000003"}, {"pts_us", 200000},
       {"box_norm", {0.1, 0.1, 0.3, 0.3}}}
  });
  write_file(staging / "spatial" / "masks.index.jsonl", "");
  write_file(staging / "spatial" / "depth.index.jsonl", "");

  write_jsonl(staging / "text" / "text_regions.jsonl", {});
  write_jsonl(staging / "text" / "text_observations.jsonl", {});
  write_jsonl(staging / "text" / "numeric_values.jsonl", {});
  write_json(staging / "text" / "text_absence.json", {
      {"schema_version", "svp-text-absence-v1"},
      {"ocr_required", true}, {"ocr_completed", true},
      {"text_region_count", 0}, {"text_observation_count", 0},
      {"numeric_value_count", 0}, {"reason", "no_text_detected"}
  });
  write_jsonl(staging / "text" / "evidence_crops.jsonl", {});

  write_jsonl(staging / "colors" / "color_observations.jsonl", {});
  write_json(staging / "colors" / "color_summary.json", {
      {"schema_version", "svp-color-summary-v1"},
      {"color_observation_count", 0},
      {"color_space", "svp_oklch_v1"},
      {"color_bucket_registry_version", "svp-color-buckets-v1"}
  });
  write_json(staging / "colors" / "color_absence.json", {
      {"schema_version", "svp-color-absence-v1"},
      {"color_required", true}, {"color_completed", true}
  });

  std::filesystem::create_directories(staging / "embeddings");
  write_json(staging / "embeddings" / "embedding_sets.json", {{"schema_version", "svp-embedding-sets-v1"}});
  write_file(staging / "embeddings" / "embeddings.index.jsonl", "");

  std::filesystem::create_directories(staging / "index");
  write_json(staging / "index" / "index_manifest.json", {
      {"index_schema_version", "svp-index-v1"},
      {"sqlite_file", "index.sqlite"},
      {"logical_row_stream_version", "1"},
      {"table_count", 6}, {"row_count", 10}
  });

  std::filesystem::create_directories(staging / "provenance");
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

  const auto summary = svp::package::write_relationships_and_provenance(staging);
  assert(summary.relationships_written > 0);

  // Also write spatial relationships that require mask data directly,
  // since this fixture has empty masks.index.jsonl.
  {
    std::ofstream rel_out(staging / "relationships" / "relationships.jsonl",
                          std::ios::app);
    rel_out << nlohmann::json{{"id", "rel_overlaps_1"}, {"type", "overlaps"},
        {"source_id", "region_000001"}, {"target_id", "region_000002"},
        {"start_us", 0}, {"end_us", 0}, {"confidence", 0.8}}.dump() << "\n";
    rel_out << nlohmann::json{{"id", "rel_contains_1"}, {"type", "contains"},
        {"source_id", "region_000004"}, {"target_id", "region_000005"},
        {"start_us", 100000}, {"end_us", 100000}, {"confidence", 0.9}}.dump() << "\n";
    rel_out << nlohmann::json{{"id", "rel_near_1"}, {"type", "near"},
        {"source_id", "region_000001"}, {"target_id", "region_000003"},
        {"start_us", 0}, {"end_us", 0}, {"confidence", 0.7}}.dump() << "\n";
  }

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}
