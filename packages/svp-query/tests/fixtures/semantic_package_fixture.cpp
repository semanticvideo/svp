#include "semantic_package_fixture.hpp"
#include "../query_test_io.hpp"

#include "svp/package/package_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"

#include <cassert>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

std::filesystem::path create_semantic_test_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-semantic-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_semantic.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_semantic_test_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  nlohmann::json transcript_json = {
      {"language", {{"primary", "en"}, {"detected", {"en"}}, {"mode", "single"}, {"confidence", 0.94}}},
      {"duration_us", 30000000},
      {"word_count", 3},
      {"speaker_count", 1},
      {"source_audio_id", "astream_analysis_0001"},
      {"processor_id", "proc_whispercpp_0001"}
  };
  write_json(staging / "transcript" / "transcript.json", transcript_json);

  std::vector<nlohmann::json> words = {
      {{"id", "word_000001"}, {"text", "hello"}, {"normalized_text", "hello"},
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}},
      {{"id", "word_000002"}, {"text", "world"}, {"normalized_text", "world"},
       {"start_us", 500000}, {"end_us", 1000000}, {"speaker_id", "speaker_0001"}},
      {{"id", "word_000003"}, {"text", "camera"}, {"normalized_text", "camera"},
       {"start_us", 1000000}, {"end_us", 1500000}, {"speaker_id", "speaker_0001"}},
  };
  write_jsonl(staging / "transcript" / "words.jsonl", words);

  std::vector<nlohmann::json> speakers = {
      {{"id", "speaker_0001"}, {"display_name", "Speaker 1"},
       {"total_speech_us", 2500000}, {"confidence", 0.91},
       {"processor_id", "proc_whispercpp_0001"}}
  };
  write_jsonl(staging / "transcript" / "speakers.jsonl", speakers);

  std::vector<nlohmann::json> speaker_segments = {
      {{"segment_id", "seg_001"}, {"speaker_id", "speaker_0001"},
       {"start_us", 0}, {"end_us", 1500000}}
  };
  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", speaker_segments);
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});

  write_jsonl(staging / "timeline" / "frames.jsonl", {
      {{"id", "frame_000001"}, {"pts_us", 0}, {"shot_id", "shot_000001"}},
      {{"id", "frame_000002"}, {"pts_us", 500000}, {"shot_id", "shot_000001"}}
  });
  write_jsonl(staging / "timeline" / "shots.jsonl", {
      {{"id", "shot_000001"}, {"start_us", 0}, {"end_us", 15000000}}
  });
  write_jsonl(staging / "timeline" / "scenes.jsonl", {
      {{"id", "scene_000001"}, {"start_us", 0}, {"end_us", 30000000},
       {"shot_ids", {"shot_000001"}}}
  });

  write_jsonl(staging / "entities" / "entities.jsonl", {
      {{"id", "entity_001"}, {"entity_type", "person"}, {"label", "Person 1"}}
  });
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  std::filesystem::create_directories(staging / "spatial");
  write_jsonl(staging / "spatial" / "regions.jsonl", {
      {{"id", "region_000001"}, {"entity_id", "entity_001"},
       {"frame_id", "frame_000001"}, {"pts_us", 500000}}
  });
  write_file(staging / "spatial" / "masks.index.jsonl", "");
  write_file(staging / "spatial" / "depth.index.jsonl", "");

  write_jsonl(staging / "text" / "text_regions.jsonl", {
      {{"text_region_id", "text_region_000001"},
       {"observation_type", "text_detection"},
       {"start_us", 1000000}, {"end_us", 2000000},
       {"shot_id", "shot_000001"}, {"scene_id", "scene_000001"}}
  });
  write_jsonl(staging / "text" / "text_observations.jsonl", {});
  write_jsonl(staging / "text" / "numeric_values.jsonl", {});
  write_json(staging / "text" / "text_absence.json", {
      {"schema_version", "svp-text-absence-v1"},
      {"ocr_required", true}, {"ocr_completed", true},
      {"text_region_count", 1}, {"text_observation_count", 0},
      {"numeric_value_count", 0}, {"reason", "text_detected"}
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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}

std::filesystem::path create_zero_pts_test_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-zero-pts-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_zero_pts.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_zero_pts_test_pkg"},
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

  std::vector<nlohmann::json> words = {
      {{"id", "word_000001"}, {"text", "hello"}, {"normalized_text", "hello"},
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}},
  };
  write_jsonl(staging / "transcript" / "words.jsonl", words);

  std::vector<nlohmann::json> speakers = {
      {{"id", "speaker_0001"}, {"display_name", "Speaker 1"},
       {"total_speech_us", 500000}, {"confidence", 0.91},
       {"processor_id", "proc_whispercpp_0001"}}
  };
  write_jsonl(staging / "transcript" / "speakers.jsonl", speakers);

  std::vector<nlohmann::json> speaker_segments = {
      {{"segment_id", "seg_001"}, {"speaker_id", "speaker_0001"},
       {"start_us", 0}, {"end_us", 500000}}
  };
  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", speaker_segments);
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});

  write_jsonl(staging / "timeline" / "frames.jsonl", {
      {{"id", "frame_000001"}, {"pts_us", 0}, {"shot_id", "shot_000001"}}
  });
  write_jsonl(staging / "timeline" / "shots.jsonl", {
      {{"id", "shot_000001"}, {"start_us", 0}, {"end_us", 15000000}}
  });
  write_jsonl(staging / "timeline" / "scenes.jsonl", {
      {{"id", "scene_000001"}, {"start_us", 0}, {"end_us", 30000000},
       {"shot_ids", {"shot_000001"}}}
  });

  write_jsonl(staging / "entities" / "entities.jsonl", {
      {{"id", "entity_001"}, {"entity_type", "person"}, {"label", "Person 1"}}
  });
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  std::filesystem::create_directories(staging / "spatial");
  write_jsonl(staging / "spatial" / "regions.jsonl", {
      {{"id", "region_000001"}, {"entity_id", "entity_001"},
       {"frame_id", "frame_000001"}, {"pts_us", 0}}
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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}
