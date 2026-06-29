#include "malformed_package_fixture.hpp"
#include "../query_test_io.hpp"

#include "svp/package/package_writer.hpp"

#include <cassert>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

std::filesystem::path create_malformed_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-malformed-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "malformed.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_malformed_test"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  // Malformed transcript.json (invalid JSON)
  write_file(staging / "transcript" / "transcript.json", "{not valid json}");

  // Malformed words.jsonl (line 2 is broken)
  write_file(staging / "transcript" / "words.jsonl",
             "{\"id\":\"word_000001\",\"text\":\"hello\"}\n"
             "this is not json\n"
             "{\"id\":\"word_000002\",\"text\":\"world\"}\n");

  write_jsonl(staging / "transcript" / "speakers.jsonl", {});
  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", {});
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});

  // Timeline
  write_jsonl(staging / "timeline" / "frames.jsonl", {});
  write_jsonl(staging / "timeline" / "shots.jsonl", {});
  write_jsonl(staging / "timeline" / "scenes.jsonl", {});

  // Entities
  write_jsonl(staging / "entities" / "entities.jsonl", {});
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  // Spatial
  std::filesystem::create_directories(staging / "spatial");
  write_jsonl(staging / "spatial" / "regions.jsonl", {});
  write_file(staging / "spatial" / "masks.index.jsonl", "");
  write_file(staging / "spatial" / "depth.index.jsonl", "");

  // Text (valid)
  write_jsonl(staging / "text" / "text_regions.jsonl", {});
  write_jsonl(staging / "text" / "text_observations.jsonl", {});
  write_jsonl(staging / "text" / "numeric_values.jsonl", {});
  write_json(staging / "text" / "text_absence.json", {{"reason", "test"}});
  write_jsonl(staging / "text" / "evidence_crops.jsonl", {});

  // Colors (valid)
  write_jsonl(staging / "colors" / "color_observations.jsonl", {});
  write_json(staging / "colors" / "color_summary.json", {{"count", 0}});
  write_json(staging / "colors" / "color_absence.json", {{"reason", "test"}});

  // Relationships
  std::filesystem::create_directories(staging / "relationships");
  write_jsonl(staging / "relationships" / "relationships.jsonl", {});

  // Embeddings
  std::filesystem::create_directories(staging / "embeddings");
  write_json(staging / "embeddings" / "embedding_sets.json", {{"v", 1}});
  write_file(staging / "embeddings" / "embeddings.index.jsonl", "");

  // Index
  std::filesystem::create_directories(staging / "index");
  write_json(staging / "index" / "index_manifest.json", {{"v", 1}});

  // Malformed validation.json (invalid JSON)
  write_file(staging / "provenance" / "validation.json", "<<<not json>>>");
  write_json(staging / "provenance" / "build.json", {{"build_id", "test"}});
  write_jsonl(staging / "provenance" / "processors.jsonl", {});
  write_jsonl(staging / "provenance" / "input_hashes.jsonl", {});
  write_jsonl(staging / "provenance" / "model_hashes.jsonl", {});

  // Media
  std::filesystem::create_directories(staging / "media" / "original");
  std::filesystem::create_directories(staging / "media" / "audio");

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}
