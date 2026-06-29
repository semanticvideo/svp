#include "query_test_registry.hpp"
#include "query_test_io.hpp"
#include "svp/query/traversal.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

void test_traversal_missing_relationships() {
  // Create a valid package without relationships.jsonl in the ZIP.
  // We build the staging dir without creating the relationships file,
  // so write_package_skeleton will not include it in the archive.
  const auto root = std::filesystem::temp_directory_path() / "svp-query-missing-rel-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_missing_rel.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_query_missing_rel_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  // Minimal staging — same as create_test_package but WITHOUT relationships.jsonl.
  write_jsonl(staging / "transcript" / "words.jsonl", {
      {{"id", "word_000001"}, {"text", "hello"}, {"normalized_text", "hello"},
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}}
  });
  write_jsonl(staging / "transcript" / "speakers.jsonl", {});
  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", {});
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});
  write_jsonl(staging / "timeline" / "frames.jsonl", {});
  write_jsonl(staging / "timeline" / "shots.jsonl", {});
  write_jsonl(staging / "timeline" / "scenes.jsonl", {});
  write_jsonl(staging / "entities" / "entities.jsonl", {});
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});
  std::filesystem::create_directories(staging / "spatial");
  write_file(staging / "spatial" / "regions.jsonl", "");
  write_file(staging / "spatial" / "masks.index.jsonl", "");
  write_file(staging / "spatial" / "depth.index.jsonl", "");
  write_jsonl(staging / "text" / "text_regions.jsonl", {});
  write_jsonl(staging / "text" / "text_observations.jsonl", {});
  write_jsonl(staging / "text" / "numeric_values.jsonl", {});
  write_jsonl(staging / "text" / "evidence_crops.jsonl", {});
  write_json(staging / "text" / "text_absence.json", {
      {"schema_version", "svp-text-absence-v1"},
      {"ocr_required", true}, {"ocr_completed", true},
      {"text_region_count", 0}, {"text_observation_count", 0},
      {"reason", "no_text_detected"}
  });
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

  // Deliberately do NOT create relationships/relationships.jsonl.

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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);

  // Verify the package does not contain relationships.jsonl.
  const auto layout = svp::package::read_package_layout(package_path);
  assert(layout.has_value());
  assert(!layout.value().has_entry("relationships/relationships.jsonl"));

  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(package_path, opts);

  // Contract: missing relationships file = error.
  assert(!result.error_message.empty());
  assert(result.edges.empty());

  std::cout << "test_traversal_missing_relationships: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_missing_relationships)

void test_traversal_malformed_relationships() {
  // Create a package with malformed relationships.jsonl content.
  const auto root = std::filesystem::temp_directory_path() / "svp-query-malformed-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_malformed.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_query_malformed_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  // Minimal staging with malformed relationships
  write_jsonl(staging / "transcript" / "words.jsonl", {
      {{"id", "word_000001"}, {"text", "hello"}, {"normalized_text", "hello"},
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}}
  });
  write_jsonl(staging / "transcript" / "speakers.jsonl", {});
  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", {});
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});
  write_jsonl(staging / "timeline" / "frames.jsonl", {});
  write_jsonl(staging / "timeline" / "shots.jsonl", {});
  write_jsonl(staging / "timeline" / "scenes.jsonl", {});
  write_jsonl(staging / "entities" / "entities.jsonl", {});
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});
  std::filesystem::create_directories(staging / "spatial");
  write_file(staging / "spatial" / "regions.jsonl", "");
  write_file(staging / "spatial" / "masks.index.jsonl", "");
  write_file(staging / "spatial" / "depth.index.jsonl", "");
  write_jsonl(staging / "text" / "text_regions.jsonl", {});
  write_jsonl(staging / "text" / "text_observations.jsonl", {});
  write_jsonl(staging / "text" / "numeric_values.jsonl", {});
  write_jsonl(staging / "text" / "evidence_crops.jsonl", {});
  write_json(staging / "text" / "text_absence.json", {
      {"schema_version", "svp-text-absence-v1"},
      {"ocr_required", true}, {"ocr_completed", true},
      {"text_region_count", 0}, {"text_observation_count", 0},
      {"reason", "no_text_detected"}
  });
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

  // Write malformed relationships JSONL
  std::filesystem::create_directories(staging / "relationships");
  {
    std::ofstream rel_file(staging / "relationships" / "relationships.jsonl");
    rel_file << "this is not valid json\n";
    rel_file << "{\"type\":\"word_spoken_by\",\"source_id\":\"word_000001\",\"target_id\":\"speaker_0001\",\"id\":\"rel_001\"}\n";
    rel_file << "{broken json line\n";
  }

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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);

  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(package_path, opts);

  // Contract: malformed JSONL produces an error message.
  assert(!result.error_message.empty());
  assert(result.edges.empty());

  std::cout << "test_traversal_malformed_relationships: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_malformed_relationships)
