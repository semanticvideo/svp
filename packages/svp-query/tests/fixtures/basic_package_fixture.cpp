#include "basic_package_fixture.hpp"
#include "../query_test_io.hpp"

#include "svp/package/package_writer.hpp"

#include <cassert>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

std::filesystem::path create_test_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_query_test_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  // Transcript
  nlohmann::json transcript_json = {
      {"language", {{"primary", "en"}, {"detected", {"en"}}, {"mode", "single"}, {"confidence", 0.94}}},
      {"duration_us", 30000000},
      {"word_count", 5},
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
      {{"id", "word_000004"}, {"text", "action"}, {"normalized_text", "action"},
       {"start_us", 1500000}, {"end_us", 2000000}, {"speaker_id", "speaker_0001"}},
      {{"id", "word_000005"}, {"text", "cut"}, {"normalized_text", "cut"},
       {"start_us", 2000000}, {"end_us", 2500000}, {"speaker_id", "speaker_0001"}}
  };
  write_jsonl(staging / "transcript" / "words.jsonl", words);

  std::vector<nlohmann::json> speakers = {
      {{"id", "speaker_0001"}, {"display_name", "Speaker 1"},
       {"total_speech_us", 2500000}, {"confidence", 0.91},
       {"processor_id", "proc_whispercpp_0001"}}
  };
  write_jsonl(staging / "transcript" / "speakers.jsonl", speakers);

  write_jsonl(staging / "transcript" / "speaker_segments.jsonl", {});
  write_jsonl(staging / "transcript" / "speech_regions.jsonl", {});

  // Timeline
  write_jsonl(staging / "timeline" / "frames.jsonl", {});
  write_jsonl(staging / "timeline" / "shots.jsonl", {
      {{"id", "shot_000001"}, {"start_us", 0}, {"end_us", 15000000}}
  });
  write_jsonl(staging / "timeline" / "scenes.jsonl", {
      {{"id", "scene_000001"}, {"start_us", 0}, {"end_us", 30000000},
       {"shot_ids", {"shot_000001"}}}
  });

  // Entities
  write_jsonl(staging / "entities" / "entities.jsonl", {});
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  // Spatial
  std::filesystem::create_directories(staging / "spatial");
  write_jsonl(staging / "spatial" / "regions.jsonl", {});
  write_file(staging / "spatial" / "masks.index.jsonl", "");
  write_file(staging / "spatial" / "depth.index.jsonl", "");

  // Text
  std::vector<nlohmann::json> text_regions = {
      {{"text_region_id", "text_region_000001"},
       {"observation_type", "text_detection"},
       {"start_us", 1000000}, {"end_us", 2000000},
       {"shot_id", "shot_000001"}, {"scene_id", "scene_000001"}}
  };
  write_jsonl(staging / "text" / "text_regions.jsonl", text_regions);

  std::vector<nlohmann::json> text_observations = {
      {{"text_observation_id", "text_obs_000001"},
       {"text_region_id", "text_region_000001"},
       {"observation_type", "text_recognition"},
       {"raw_text", "SALE $9.99"},
       {"normalized_text", "sale 9.99"},
       {"confidence", 0.902},
       {"evidence_crop_refs", {"crop_000001"}}},
      {{"text_observation_id", "text_obs_000002"},
       {"text_region_id", "text_region_000002"},
       {"observation_type", "ui_text"},
       {"raw_text", "Settings"},
       {"normalized_text", "settings"},
       {"confidence", 0.88}}
  };
  write_jsonl(staging / "text" / "text_observations.jsonl", text_observations);

  std::vector<nlohmann::json> numeric_values = {
      {{"numeric_value_id", "numeric_value_000001"},
       {"text_observation_id", "text_obs_000001"},
       {"raw_text", "$9.99"},
       {"normalized_text", "9.99"},
       {"number_kind", "decimal"},
       {"numeric_value", "9.99"},
       {"unit", "currency_unknown"},
       {"confidence", 0.89}}
  };
  write_jsonl(staging / "text" / "numeric_values.jsonl", numeric_values);

  write_json(staging / "text" / "text_absence.json", {
      {"schema_version", "svp-text-absence-v1"},
      {"ocr_required", true}, {"ocr_completed", true},
      {"text_region_count", 1}, {"text_observation_count", 2},
      {"numeric_value_count", 1}, {"reason", "text_detected"}
  });

  std::vector<nlohmann::json> evidence_crops = {
      {{"crop_id", "crop_000001"},
       {"text_region_id", "text_region_000001"},
       {"crop_file_path", "text/evidence_crops/crop_000001.jpg"},
       {"crop_size_bytes", 1024},
       {"image_format", "jpeg"}}
  };
  write_jsonl(staging / "text" / "evidence_crops.jsonl", evidence_crops);

  // Colors
  std::vector<nlohmann::json> color_observations = {
      {{"color_observation_id", "color_obs_000001"},
       {"target_type", "shot"}, {"target_id", "shot_000001"},
       {"start_us", 0}, {"end_us", 15000000},
       {"sampling_basis", "keyframe_full_frame"},
       {"color_space", "svp_oklch_v1"},
       {"color_bucket_registry_version", "svp-color-buckets-v1"},
       {"bucket_coverage", {{"orange", 0.70}, {"black", 0.12}, {"white", 0.08}, {"other", 0.07}}},
       {"dominant_bucket", "orange"},
       {"coverage_total", 1.0},
       {"quality_score", 0.96}},
      {{"color_observation_id", "color_obs_000002"},
       {"target_type", "scene"}, {"target_id", "scene_000001"},
       {"start_us", 0}, {"end_us", 30000000},
       {"sampling_basis", "keyframe_full_frame"},
       {"color_space", "svp_oklch_v1"},
       {"color_bucket_registry_version", "svp-color-buckets-v1"},
       {"bucket_coverage", {{"blue", 0.45}, {"black", 0.30}, {"white", 0.15}, {"other", 0.10}}},
       {"dominant_bucket", "blue"},
       {"coverage_total", 1.0},
       {"quality_score", 0.92}}
  };
  write_jsonl(staging / "colors" / "color_observations.jsonl", color_observations);

  write_json(staging / "colors" / "color_summary.json", {
      {"schema_version", "svp-color-summary-v1"},
      {"color_observation_count", 2},
      {"color_space", "svp_oklch_v1"},
      {"color_bucket_registry_version", "svp-color-buckets-v1"}
  });

  write_json(staging / "colors" / "color_absence.json", {
      {"schema_version", "svp-color-absence-v1"},
      {"color_required", true}, {"color_completed", true}
  });

  // Relationships
  std::filesystem::create_directories(staging / "relationships");
  write_jsonl(staging / "relationships" / "relationships.jsonl", {});

  // Embeddings
  std::filesystem::create_directories(staging / "embeddings");
  write_json(staging / "embeddings" / "embedding_sets.json", {{"schema_version", "svp-embedding-sets-v1"}});
  write_file(staging / "embeddings" / "embeddings.index.jsonl", "");

  // Index
  std::filesystem::create_directories(staging / "index");
  write_json(staging / "index" / "index_manifest.json", {
      {"index_schema_version", "svp-index-v1"},
      {"sqlite_file", "index.sqlite"},
      {"logical_row_stream_version", "1"},
      {"table_count", 6}, {"row_count", 10}
  });

  // Provenance
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

  // Media
  std::filesystem::create_directories(staging / "media" / "original");
  std::filesystem::create_directories(staging / "media" / "audio");

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}
