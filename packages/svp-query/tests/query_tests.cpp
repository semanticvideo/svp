#include "svp/query/query_reader.hpp"
#include "svp/query/query_ops.hpp"
#include "svp/query/traversal.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_set>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& r : records) {
    out << r.dump() << "\n";
  }
}

void write_json(const std::filesystem::path& path, const nlohmann::json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << value.dump(2) << "\n";
}

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

void test_list_layers() {
  const auto pkg = create_test_package();
  auto summary = svp::query::list_layers(pkg);

  assert(!summary.layers.empty());
  assert(summary.total_entries > 0);

  bool found_words = false;
  bool found_colors = false;
  bool found_validation = false;
  for (const auto& layer : summary.layers) {
    if (layer.entry == "transcript/words.jsonl" && layer.present) {
      assert(layer.record_count == 5);
      found_words = true;
    }
    if (layer.entry == "colors/color_observations.jsonl" && layer.present) {
      assert(layer.record_count == 2);
      found_colors = true;
    }
    if (layer.entry == "provenance/validation.json" && layer.present) {
      assert(layer.record_count == 1);
      found_validation = true;
    }
  }
  assert(found_words);
  assert(found_colors);
  assert(found_validation);

  std::cout << "test_list_layers: passed\n";
}

void test_transcript_summary() {
  const auto pkg = create_test_package();
  auto result = svp::query::transcript_summary(pkg);

  assert(result.present);
  assert(result.word_count_file == 5);
  assert(result.speaker_count_file == 1);

  const auto lang = result.transcript_json.value("language", nlohmann::json{});
  assert(lang.value("primary", "") == "en");

  std::cout << "test_transcript_summary: passed\n";
}

void test_find_words() {
  const auto pkg = create_test_package();

  auto matches = svp::query::find_words(pkg, "cam", 100);
  assert(matches.size() == 1);
  assert(matches[0].record.value("text", "") == "camera");

  auto matches2 = svp::query::find_words(pkg, "o", 100);
  assert(matches2.size() == 3);

  auto matches3 = svp::query::find_words(pkg, "nonexistent", 100);
  assert(matches3.empty());

  std::cout << "test_find_words: passed\n";
}

void test_list_speakers() {
  const auto pkg = create_test_package();
  auto speakers = svp::query::list_speakers(pkg);

  assert(speakers.size() == 1);
  assert(speakers[0].record.value("id", "") == "speaker_0001");
  assert(speakers[0].word_count == 5);

  std::cout << "test_list_speakers: passed\n";
}

void test_list_ocr_observations() {
  const auto pkg = create_test_package();

  auto all = svp::query::list_ocr_observations(pkg, std::nullopt, 100);
  assert(all.size() == 2);

  auto filtered = svp::query::list_ocr_observations(pkg, std::string{"sale"}, 100);
  assert(filtered.size() == 1);
  assert(filtered[0].record.value("raw_text", "") == "SALE $9.99");

  auto with_crops = svp::query::list_ocr_observations(pkg, std::nullopt, 100);
  bool has_crop_ref = false;
  for (const auto& obs : with_crops) {
    const auto refs = obs.record.find("evidence_crop_refs");
    if (refs != obs.record.end() && refs->is_array() && !refs->empty()) {
      has_crop_ref = true;
    }
  }
  assert(has_crop_ref);

  std::cout << "test_list_ocr_observations: passed\n";
}

void test_list_color_observations() {
  const auto pkg = create_test_package();

  auto all = svp::query::list_color_observations(pkg, std::nullopt, std::nullopt, 100);
  assert(all.size() == 2);

  auto orange = svp::query::list_color_observations(pkg, std::string{"orange"}, std::nullopt, 100);
  assert(orange.size() == 1);
  assert(orange[0].record.value("dominant_bucket", "") == "orange");

  auto threshold = svp::query::list_color_observations(pkg, std::nullopt, std::make_optional(0.5), 100);
  assert(threshold.size() == 1);
  assert(threshold[0].record.value("dominant_bucket", "") == "orange");

  std::cout << "test_list_color_observations: passed\n";
}

void test_show_validation() {
  const auto pkg = create_test_package();
  auto info = svp::query::show_validation(pkg);

  assert(info.present);
  assert(info.record.value("status", "") == "valid");
  assert(info.record.value("core_status", "") == "valid");

  std::cout << "test_show_validation: passed\n";
}

void test_read_jsonl_entry() {
  const auto pkg = create_test_package();
  auto result = svp::query::read_jsonl_entry(pkg, "transcript/words.jsonl");

  assert(result.present);
  assert(result.readable);
  assert(result.records.size() == 5);
  assert(result.records[0].value("text", "") == "hello");

  auto missing = svp::query::read_jsonl_entry(pkg, "nonexistent/file.jsonl");
  assert(!missing.present);
  assert(!missing.readable);

  std::cout << "test_read_jsonl_entry: passed\n";
}

void test_read_json_entry() {
  const auto pkg = create_test_package();
  auto result = svp::query::read_json_entry(pkg, "transcript/transcript.json");

  assert(result.present);
  assert(result.readable);
  assert(result.parsed);
  assert(result.value.value("word_count", 0) == 5);

  std::cout << "test_read_json_entry: passed\n";
}

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

void test_malformed_jsonl() {
  const auto pkg = create_malformed_package();
  auto result = svp::query::read_jsonl_entry(pkg, "transcript/words.jsonl");

  assert(result.present);
  assert(result.readable);
  assert(result.has_malformed);
  assert(result.malformed_line_count == 1);
  assert(!result.error_message.empty());
  assert(result.records.size() == 2);

  // Verify error message contains useful detail
  assert(result.error_message.find("malformed") != std::string::npos);
  assert(result.error_message.find("line 2") != std::string::npos);

  // Happy path: valid JSONL should not have malformed flag
  const auto good_pkg = create_test_package();
  auto good_result = svp::query::read_jsonl_entry(good_pkg, "transcript/words.jsonl");
  assert(good_result.readable);
  assert(!good_result.has_malformed);
  assert(good_result.malformed_line_count == 0);
  assert(good_result.error_message.empty());

  std::cout << "test_malformed_jsonl: passed\n";
}

void test_malformed_transcript_json() {
  const auto pkg = create_malformed_package();
  auto result = svp::query::transcript_summary(pkg);

  assert(result.present);
  assert(!result.parsed);
  assert(!result.error_message.empty());

  // Happy path: valid transcript should parse
  const auto good_pkg = create_test_package();
  auto good_result = svp::query::transcript_summary(good_pkg);
  assert(good_result.present);
  assert(good_result.parsed);
  assert(good_result.error_message.empty());

  std::cout << "test_malformed_transcript_json: passed\n";
}

void test_malformed_validation_json() {
  const auto pkg = create_malformed_package();
  auto info = svp::query::show_validation(pkg);

  assert(info.present);
  assert(!info.parsed);
  assert(!info.error_message.empty());

  // Happy path: valid validation should parse
  const auto good_pkg = create_test_package();
  auto good_info = svp::query::show_validation(good_pkg);
  assert(good_info.present);
  assert(good_info.parsed);
  assert(good_info.error_message.empty());
  assert(good_info.record.value("status", "") == "valid");

  std::cout << "test_malformed_validation_json: passed\n";
}

void test_malformed_layers() {
  const auto pkg = create_malformed_package();
  auto summary = svp::query::list_layers(pkg);

  bool found_malformed_words = false;
  bool found_malformed_transcript = false;
  bool found_malformed_validation = false;
  bool found_mimetype_text = false;

  for (const auto& layer : summary.layers) {
    if (layer.entry == "transcript/words.jsonl") {
      assert(layer.present);
      assert(layer.has_malformed);
      assert(layer.malformed_line_count == 1);
      found_malformed_words = true;
    }
    if (layer.entry == "transcript/transcript.json") {
      assert(layer.present);
      assert(layer.has_malformed);
      found_malformed_transcript = true;
    }
    if (layer.entry == "provenance/validation.json") {
      assert(layer.present);
      assert(layer.has_malformed);
      found_malformed_validation = true;
    }
    if (layer.entry == "mimetype") {
      assert(layer.kind == "text");
      found_mimetype_text = true;
    }
  }

  assert(found_malformed_words);
  assert(found_malformed_transcript);
  assert(found_malformed_validation);
  assert(found_mimetype_text);

  std::cout << "test_malformed_layers: passed\n";
}

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

void test_relationship_summary() {
  const auto pkg = create_test_package_with_relationships();
  auto summary = svp::query::relationship_summary(pkg);

  assert(summary.present);
  assert(summary.readable);
  assert(summary.total_count == 5);
  assert(summary.support_count == 2);
  assert(summary.semantic_count == 2);
  assert(summary.unknown_count == 1);

  std::cout << "test_relationship_summary: passed\n";
}

void test_list_relationships_all() {
  const auto pkg = create_test_package_with_relationships();
  auto all = svp::query::list_relationships(pkg, std::nullopt, 100);

  assert(all.size() == 5);

  std::cout << "test_list_relationships_all: passed\n";
}

void test_list_relationships_filtered_by_support() {
  const auto pkg = create_test_package_with_relationships();
  auto support_only = svp::query::list_relationships(pkg, std::string{"support"}, 100);

  assert(support_only.size() == 2);
  for (const auto& rel : support_only) {
    assert(rel.relationship_class == "support");
  }

  std::cout << "test_list_relationships_filtered_by_support: passed\n";
}

void test_list_relationships_filtered_by_semantic() {
  const auto pkg = create_test_package_with_relationships();
  auto semantic_only = svp::query::list_relationships(pkg, std::string{"semantic"}, 100);

  assert(semantic_only.size() == 2);
  for (const auto& rel : semantic_only) {
    assert(rel.relationship_class == "semantic");
  }

  std::cout << "test_list_relationships_filtered_by_semantic: passed\n";
}

void test_list_relationships_filtered_by_unknown() {
  const auto pkg = create_test_package_with_relationships();
  auto unknown_only = svp::query::list_relationships(pkg, std::string{"unknown"}, 100);

  assert(unknown_only.size() == 1);
  assert(unknown_only[0].relationship_class == "unknown");
  assert(unknown_only[0].record.value("type", "") == "totally_unknown_type");

  std::cout << "test_list_relationships_filtered_by_unknown: passed\n";
}

void test_json_type_field_canonical_in_query() {
  const auto pkg = create_test_package_with_relationships();
  auto all = svp::query::list_relationships(pkg, std::nullopt, 100);

  for (const auto& rel : all) {
    assert(rel.record.contains("type"));
    assert(!rel.record.contains("relationship_type"));
  }

  std::cout << "test_json_type_field_canonical_in_query: passed\n";
}

std::filesystem::path create_traversal_test_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-traversal-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_traversal.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_traversal_test_pkg"},
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
      {{"id", "frame_000002"}, {"pts_us", 50000}, {"shot_id", "shot_000001"}}
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
      {{"id", "entity_002"}, {"entity_type", "object"}, {"label", "Object 1"}}
  });
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  std::filesystem::create_directories(staging / "spatial");
  write_jsonl(staging / "spatial" / "regions.jsonl", {});
  write_jsonl(staging / "spatial" / "masks.index.jsonl", {
      {{"mask_id", "mask_000001"}, {"block_id", "block_mask_001"},
       {"frame_id", "frame_000001"},
       {"block_path", "spatial/masks/mask_000001.bin"},
       {"block_size_bytes", 2048}}
  });
  write_jsonl(staging / "spatial" / "depth.index.jsonl", {
      {{"depth_frame_id", "depth_frame_000001"}, {"block_id", "block_depth_001"},
       {"frame_id", "frame_000001"},
       {"block_path", "spatial/depth/depth_000001.bin"},
       {"block_size_bytes", 4096}}
  });

  write_jsonl(staging / "text" / "text_regions.jsonl", {
      {{"text_region_id", "text_region_000001"},
       {"observation_type", "text_detection"},
       {"start_us", 1000000}, {"end_us", 2000000},
       {"shot_id", "shot_000001"}, {"scene_id", "scene_000001"}}
  });
  write_jsonl(staging / "text" / "text_observations.jsonl", {
      {{"text_observation_id", "text_obs_000001"},
       {"text_region_id", "text_region_000001"},
       {"observation_type", "text_recognition"},
       {"raw_text", "SALE $9.99"},
       {"normalized_text", "sale 9.99"},
       {"confidence", 0.902},
       {"evidence_crop_refs", {"crop_000001"}}}
  });
  write_jsonl(staging / "text" / "numeric_values.jsonl", {
      {{"numeric_value_id", "numeric_value_000001"},
       {"text_observation_id", "text_obs_000001"},
       {"raw_text", "$9.99"},
       {"normalized_text", "9.99"},
       {"number_kind", "decimal"},
       {"numeric_value", "9.99"},
       {"unit", "currency_unknown"},
       {"confidence", 0.89}}
  });
  write_json(staging / "text" / "text_absence.json", {
      {"schema_version", "svp-text-absence-v1"},
      {"ocr_required", true}, {"ocr_completed", true},
      {"text_region_count", 1}, {"text_observation_count", 1},
      {"numeric_value_count", 1}, {"reason", "text_detected"}
  });
  write_jsonl(staging / "text" / "evidence_crops.jsonl", {
      {{"crop_id", "crop_000001"},
       {"text_region_id", "text_region_000001"},
       {"crop_file_path", "text/evidence_crops/crop_000001.jpg"},
       {"crop_size_bytes", 1024},
       {"image_format", "jpeg"}}
  });

  write_jsonl(staging / "colors" / "color_observations.jsonl", {
      {{"color_observation_id", "color_obs_000001"},
       {"target_type", "shot"}, {"target_id", "shot_000001"},
       {"start_us", 0}, {"end_us", 15000000},
       {"sampling_basis", "keyframe_full_frame"},
       {"color_space", "svp_oklch_v1"},
       {"color_bucket_registry_version", "svp-color-buckets-v1"},
       {"bucket_coverage", {{"orange", 0.70}, {"black", 0.12}, {"white", 0.08}, {"other", 0.07}}},
       {"dominant_bucket", "orange"},
       {"coverage_total", 1.0},
       {"quality_score", 0.96}}
  });
  write_json(staging / "colors" / "color_summary.json", {
      {"schema_version", "svp-color-summary-v1"},
      {"color_observation_count", 1},
      {"color_space", "svp_oklch_v1"},
      {"color_bucket_registry_version", "svp-color-buckets-v1"}
  });
  write_json(staging / "colors" / "color_absence.json", {
      {"schema_version", "svp-color-absence-v1"},
      {"color_required", true}, {"color_completed", true}
  });

  std::vector<nlohmann::json> relationships = {
      {{"id", "rel_001"}, {"type", "word_spoken_by"}, {"source_id", "word_000001"},
       {"target_id", "speaker_0001"}, {"start_us", 0}, {"end_us", 500000},
       {"confidence", 0.95}, {"processor_id", "proc_test"}},
      {{"id", "rel_002"}, {"type", "word_spoken_by"}, {"source_id", "word_000002"},
       {"target_id", "speaker_0001"}, {"start_us", 500000}, {"end_us", 1000000},
       {"confidence", 0.95}, {"processor_id", "proc_test"}},
      {{"id", "rel_003"}, {"type", "word_in_speaker_segment"}, {"source_id", "word_000001"},
       {"target_id", "seg_001"}, {"start_us", 0}, {"end_us", 500000},
       {"confidence", 1.0}, {"processor_id", "proc_test"}},
      {{"id", "rel_004"}, {"type", "observation_in_region"}, {"source_id", "text_obs_000001"},
       {"target_id", "text_region_000001"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 0.9}, {"processor_id", "proc_test"}},
      {{"id", "rel_005"}, {"type", "has_evidence_crop"}, {"source_id", "text_obs_000001"},
       {"target_id", "crop_000001"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 1.0}, {"processor_id", "proc_test"}},
      {{"id", "rel_006"}, {"type", "numeric_value_from_observation"}, {"source_id", "numeric_value_000001"},
       {"target_id", "text_obs_000001"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 0.89}, {"processor_id", "proc_test"}},
      {{"id", "rel_007"}, {"type", "color_observation_of"}, {"source_id", "color_obs_000001"},
       {"target_id", "shot_000001"}, {"start_us", 0}, {"end_us", 15000000},
       {"confidence", 0.96}, {"processor_id", "proc_test"}},
      {{"id", "rel_008"}, {"type", "appears_in_frame"}, {"source_id", "entity_001"},
       {"target_id", "frame_000001"}, {"start_us", 0}, {"end_us", 0},
       {"confidence", 1.0}, {"processor_id", "proc_test"}},
      {{"id", "rel_009"}, {"type", "appears_in_shot"}, {"source_id", "entity_001"},
       {"target_id", "shot_000001"}, {"start_us", 0}, {"end_us", 5000000},
       {"confidence", 0.88}, {"processor_id", "proc_test"}},
      {{"id", "rel_010"}, {"type", "overlaps"}, {"source_id", "entity_001"},
       {"target_id", "entity_002"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 0.79}, {"processor_id", "proc_test"}},
      {{"id", "rel_011"}, {"type", "overlaps"}, {"source_id", "entity_002"},
       {"target_id", "entity_001"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 0.79}, {"processor_id", "proc_test"}},
      {{"id", "rel_012"}, {"type", "totally_unknown_type"}, {"source_id", "entity_001"},
       {"target_id", "entity_002"}, {"start_us", 0}, {"end_us", 1000000},
       {"confidence", 0.5}, {"processor_id", "proc_test"}},
      {{"id", "rel_013"}, {"type", "word_spoken_by"}, {"source_id", "word_000003"},
       {"target_id", "nonexistent_speaker"}, {"start_us", 1000000}, {"end_us", 1500000},
       {"confidence", 0.95}, {"processor_id", "proc_test"}},
      {{"id", "rel_014"}, {"type", "embedding_source_is"}, {"source_id", "embed_text_obs_000001"},
       {"target_id", "text_obs_000001"}, {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 1.0}, {"processor_id", "proc_test"}},
  };
  write_jsonl(staging / "relationships" / "relationships.jsonl", relationships);

  std::filesystem::create_directories(staging / "embeddings");
  write_json(staging / "embeddings" / "embedding_sets.json", {{"schema_version", "svp-embedding-sets-v1"}});
  write_jsonl(staging / "embeddings" / "embeddings.index.jsonl", {
      {{"id", "embed_text_obs_000001"}, {"embedding_id", "embed_000001"},
       {"embedding_set_id", "embed_set_001"},
       {"source_type", "text_observation"}, {"source_id", "text_obs_000001"},
       {"block_path", "embeddings/embed_000001.bin"},
       {"block_size_bytes", 512}}
  });

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
  assert(std::filesystem::exists(package_path));

  return package_path;
}

void test_traversal_outgoing() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(result.start_id == "word_000001");
  assert(result.visited_node_count >= 2);

  bool found_speaker = false;
  bool found_seg = false;
  for (const auto& node : result.nodes) {
    if (node.object_id == "speaker_0001") found_speaker = true;
    if (node.object_id == "seg_001") found_seg = true;
  }
  assert(found_speaker);
  assert(found_seg);

  bool found_outgoing = false;
  bool found_incoming = false;
  for (const auto& edge : result.edges) {
    if (edge.direction == "outgoing") found_outgoing = true;
    if (edge.direction == "incoming") found_incoming = true;
  }
  assert(found_outgoing);
  assert(!found_incoming);

  std::cout << "test_traversal_outgoing: passed\n";
}

void test_traversal_incoming() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "speaker_0001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Incoming;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(result.visited_node_count >= 2);

  bool found_word1 = false;
  bool found_word2 = false;
  for (const auto& node : result.nodes) {
    if (node.object_id == "word_000001") found_word1 = true;
    if (node.object_id == "word_000002") found_word2 = true;
  }
  assert(found_word1);
  assert(found_word2);

  bool found_incoming = false;
  bool found_outgoing = false;
  for (const auto& edge : result.edges) {
    if (edge.direction == "incoming") found_incoming = true;
    if (edge.direction == "outgoing") found_outgoing = true;
  }
  assert(found_incoming);
  assert(!found_outgoing);

  std::cout << "test_traversal_incoming: passed\n";
}

void test_traversal_both_directions() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool has_outgoing = false;
  bool has_incoming = false;
  for (const auto& edge : result.edges) {
    if (edge.direction == "outgoing") has_outgoing = true;
    if (edge.direction == "incoming") has_incoming = true;
  }
  assert(has_outgoing);
  assert(has_incoming);

  std::cout << "test_traversal_both_directions: passed\n";
}

void test_traversal_max_depth() {
  const auto pkg = create_traversal_test_package();

  svp::query::TraversalOptions opts1;
  opts1.start_id = "word_000001";
  opts1.max_depth = 1;
  opts1.direction = svp::query::TraversalDirection::Both;
  opts1.limit = 100;
  auto result1 = svp::query::traverse_relationships(pkg, opts1);

  for (const auto& node : result1.nodes) {
    assert(node.depth <= 1);
  }

  svp::query::TraversalOptions opts2;
  opts2.start_id = "word_000001";
  opts2.max_depth = 0;
  opts2.direction = svp::query::TraversalDirection::Both;
  opts2.limit = 100;
  auto result2 = svp::query::traverse_relationships(pkg, opts2);

  assert(result2.visited_node_count == 1);
  assert(result2.edges.empty());

  std::cout << "test_traversal_max_depth: passed\n";
}

void test_traversal_cycle_safety() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 10;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 1000;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(result.visited_node_count > 0);

  std::unordered_set<std::string> seen;
  for (const auto& node : result.nodes) {
    assert(seen.find(node.object_id) == seen.end());
    seen.insert(node.object_id);
  }

  std::cout << "test_traversal_cycle_safety: passed\n";
}

void test_traversal_class_filter() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "support";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  for (const auto& edge : result.edges) {
    assert(edge.relationship_class == "support");
  }

  bool found_appears_in_frame = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "appears_in_frame") found_appears_in_frame = true;
  }
  assert(found_appears_in_frame);

  for (const auto& edge : result.edges) {
    assert(edge.relationship_type != "overlaps");
    assert(edge.relationship_type != "appears_in_shot");
  }

  std::cout << "test_traversal_class_filter: passed\n";
}

void test_traversal_type_filter() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.type_filter = std::string{"overlaps"};
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  for (const auto& edge : result.edges) {
    assert(edge.relationship_type == "overlaps");
  }
  assert(!result.edges.empty());

  std::cout << "test_traversal_type_filter: passed\n";
}

void test_traversal_unknown_type_traversable() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "unknown";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(!result.edges.empty());
  for (const auto& edge : result.edges) {
    assert(edge.relationship_class == "unknown");
    assert(edge.relationship_type == "totally_unknown_type");
  }

  std::cout << "test_traversal_unknown_type_traversable: passed\n";
}

void test_traversal_missing_ids_reported() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000003";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(!result.missing_object_ids.empty());
  bool found_nonexistent = false;
  for (const auto& id : result.missing_object_ids) {
    if (id == "nonexistent_speaker") found_nonexistent = true;
  }
  assert(found_nonexistent);

  for (const auto& node : result.nodes) {
    if (node.object_id == "nonexistent_speaker") {
      assert(!node.resolved);
    }
  }

  std::cout << "test_traversal_missing_ids_reported: passed\n";
}

void test_traversal_embedding_id_resolves() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "text_obs_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_embed = false;
  bool embed_resolved = false;
  for (const auto& node : result.nodes) {
    if (node.object_id == "embed_text_obs_000001") {
      found_embed = true;
      embed_resolved = node.resolved;
      assert(node.source_layer == "embeddings/embeddings.index.jsonl");
    }
  }
  assert(found_embed);
  assert(embed_resolved);

  bool embed_in_missing = false;
  for (const auto& id : result.missing_object_ids) {
    if (id == "embed_text_obs_000001") embed_in_missing = true;
  }
  assert(!embed_in_missing);

  std::cout << "test_traversal_embedding_id_resolves: passed\n";
}

void test_object_catalog_annotations() {
  const auto pkg = create_traversal_test_package();
  auto catalog = svp::query::build_object_catalog(pkg);

  const char* required_ids[] = {
      "word_000001", "text_obs_000001", "text_region_000001",
      "frame_000001", "entity_001", "crop_000001",
      "mask_000001", "depth_frame_000001", "embed_text_obs_000001"
  };
  for (const auto* id : required_ids) {
    const auto* entry = catalog.find(id);
    assert(entry != nullptr);
    assert(entry->object_id == id);
    assert(!entry->source_layer.empty());
  }

  const auto word_summary = catalog.node_summary("word_000001");
  assert(word_summary.value("kind", "") == "word");
  assert(word_summary.value("text", "") == "hello");

  const auto obs_summary = catalog.node_summary("text_obs_000001");
  assert(obs_summary.value("kind", "") == "text_observation");
  assert(obs_summary.value("raw_text", "") == "SALE $9.99");

  const auto region_summary = catalog.node_summary("text_region_000001");
  assert(region_summary.value("kind", "") == "text_region");

  const auto frame_summary = catalog.node_summary("frame_000001");
  assert(frame_summary.value("kind", "") == "frame");

  const auto entity_summary = catalog.node_summary("entity_001");
  assert(entity_summary.value("kind", "") == "entity");
  assert(entity_summary.value("entity_type", "") == "person");

  const auto crop_summary = catalog.node_summary("crop_000001");
  assert(crop_summary.value("kind", "") == "evidence_crop");
  assert(crop_summary.value("crop_file_path", "") == "text/evidence_crops/crop_000001.jpg");

  const auto mask_summary = catalog.node_summary("mask_000001");
  assert(mask_summary.value("kind", "") == "mask");
  assert(mask_summary.value("block_path", "") == "spatial/masks/mask_000001.bin");

  const auto depth_summary = catalog.node_summary("depth_frame_000001");
  assert(depth_summary.value("kind", "") == "depth");
  assert(depth_summary.value("block_path", "") == "spatial/depth/depth_000001.bin");

  const auto embed_summary = catalog.node_summary("embed_text_obs_000001");
  assert(embed_summary.value("kind", "") == "embedding");
  assert(embed_summary.value("source_type", "") == "text_observation");
  assert(embed_summary.value("source_id", "") == "text_obs_000001");

  std::cout << "test_object_catalog_annotations: passed\n";
}

void test_traversal_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);
  auto json = svp::query::traversal_result_to_json(result);

  assert(json.contains("start_id"));
  assert(json.value("start_id", "") == "word_000001");
  assert(json.contains("requested_depth"));
  assert(json.contains("visited_node_count"));
  assert(json.contains("edge_count"));
  assert(json.contains("limit_applied"));
  assert(json.contains("nodes"));
  assert(json.contains("edges"));
  assert(json["nodes"].is_array());
  assert(json["edges"].is_array());

  if (!json["nodes"].empty()) {
    const auto& first_node = json["nodes"][0];
    assert(first_node.contains("object_id"));
    assert(first_node.contains("depth"));
    assert(first_node.contains("resolved"));
  }

  if (!json["edges"].empty()) {
    const auto& first_edge = json["edges"][0];
    assert(first_edge.contains("relationship_type"));
    assert(first_edge.contains("relationship_class"));
    assert(first_edge.contains("source_id"));
    assert(first_edge.contains("target_id"));
    assert(first_edge.contains("direction"));
    assert(first_edge.contains("depth"));
  }

  std::cout << "test_traversal_json_output: passed\n";
}

void test_traversal_empty_relationships() {
  const auto pkg = create_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  // Contract: empty relationships file = valid empty graph.
  // No error, no edges, start node is still visited and resolved.
  assert(result.error_message.empty());
  assert(result.edges.empty());
  assert(result.visited_node_count == 1);
  assert(result.nodes.size() == 1);
  assert(result.nodes[0].object_id == "word_000001");
  assert(result.nodes[0].resolved);

  std::cout << "test_traversal_empty_relationships: passed\n";
}

void test_traversal_missing_relationships() {
  // Create a package without relationships.jsonl in the ZIP.
  // We reuse create_test_package but the empty file gets packed as 0 bytes.
  // To test truly missing, we need a package that doesn't include the file at all.
  // Since write_package_skeleton packs all staging files, we create a package
  // and then check: the empty file IS present (0 bytes), so this tests the
  // "present but empty" path. For truly missing, we'd need to remove the entry
  // from the ZIP, which requires zip manipulation.
  //
  // Instead, test that a non-existent package path returns an error.
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;

  auto result = svp::query::traverse_relationships(
      "/nonexistent/path/to/package.svp", opts);

  assert(!result.error_message.empty());
  assert(result.edges.empty());

  std::cout << "test_traversal_missing_relationships: passed\n";
}

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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}

void test_time_window_at_us() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;
  opts.time_window.at_us = 0;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  for (const auto& edge : result.edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start <= 0 && 0 < end);
  }

  std::cout << "test_time_window_at_us: passed\n";
}

void test_time_window_start_end() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;
  opts.time_window.start_us = 0;
  opts.time_window.end_us = 500000;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  for (const auto& edge : result.edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start < 500000 && 0 < end);
  }

  std::cout << "test_time_window_start_end: passed\n";
}

void test_time_window_at_us_excludes_non_overlapping() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;
  opts.time_window.at_us = 1200000;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  for (const auto& edge : result.edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start <= 1200000 && 1200000 < end);
  }

  std::cout << "test_time_window_at_us_excludes_non_overlapping: passed\n";
}

void test_time_window_at_and_start_mutually_exclusive() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.limit = 100;
  opts.time_window.at_us = 100;
  opts.time_window.start_us = 0;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(!result.error_message.empty());
  assert(result.error_message.find("cannot specify both") != std::string::npos);

  std::cout << "test_time_window_at_and_start_mutually_exclusive: passed\n";
}

void test_shortest_path_found() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "speaker_0001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);

  assert(result.path_found);
  assert(!result.path_nodes.empty());
  assert(result.path_nodes.front().object_id == "word_000001");
  assert(result.path_nodes.back().object_id == "speaker_0001");
  assert(result.path_edges.size() == result.path_nodes.size() - 1);

  std::cout << "test_shortest_path_found: passed\n";
}

void test_shortest_path_not_found() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "nonexistent_id";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);

  assert(!result.path_found);
  assert(!result.error_message.empty());

  std::cout << "test_shortest_path_not_found: passed\n";
}

void test_shortest_path_same_node() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "word_000001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);

  assert(result.path_found);
  assert(result.path_nodes.size() == 1);
  assert(result.path_edges.empty());
  assert(result.path_nodes[0].object_id == "word_000001");

  std::cout << "test_shortest_path_same_node: passed\n";
}

void test_shortest_path_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "speaker_0001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);
  auto json = svp::query::path_result_to_json(result);

  assert(json.contains("start_id"));
  assert(json.contains("target_id"));
  assert(json.contains("path_found"));
  assert(json.value("path_found", false) == true);
  assert(json.contains("path_length"));
  assert(json.contains("nodes"));
  assert(json.contains("edges"));
  assert(json["nodes"].is_array());
  assert(json["edges"].is_array());

  std::cout << "test_shortest_path_json_output: passed\n";
}

void test_context_mode() {
  const auto pkg = create_traversal_test_package();
  auto result = svp::query::build_context(pkg, "word_000001", 100);

  assert(result.object_id == "word_000001");
  assert(result.resolved);
  assert(!result.context_edges.empty());

  bool found_speaker = false;
  for (const auto& node : result.context_nodes) {
    if (node.object_id == "speaker_0001") found_speaker = true;
  }
  assert(found_speaker);

  std::cout << "test_context_mode: passed\n";
}

void test_context_json_output() {
  const auto pkg = create_traversal_test_package();
  auto result = svp::query::build_context(pkg, "word_000001", 100);
  auto json = svp::query::context_result_to_json(result);

  assert(json.contains("object_id"));
  assert(json.contains("resolved"));
  assert(json.value("resolved", false) == true);
  assert(json.contains("context_nodes"));
  assert(json.contains("context_edges"));
  assert(json["context_nodes"].is_array());
  assert(json["context_edges"].is_array());

  std::cout << "test_context_json_output: passed\n";
}

void test_context_unresolved_object() {
  const auto pkg = create_traversal_test_package();
  auto result = svp::query::build_context(pkg, "nonexistent_id", 100);

  assert(result.object_id == "nonexistent_id");
  assert(!result.resolved);

  std::cout << "test_context_unresolved_object: passed\n";
}

void test_edge_deduplication() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  std::set<std::string> seen_edge_ids;
  for (const auto& edge : result.edges) {
    if (!edge.relationship_id.empty()) {
      assert(seen_edge_ids.find(edge.relationship_id) == seen_edge_ids.end());
      seen_edge_ids.insert(edge.relationship_id);
    }
  }

  std::cout << "test_edge_deduplication: passed\n";
}

void test_graph_health_report() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.graph_health.total_edges > 0);
  assert(result.graph_health.total_nodes > 0);
  assert(result.graph_health.class_counts.find("support") != result.graph_health.class_counts.end() ||
         result.graph_health.class_counts.find("semantic") != result.graph_health.class_counts.end());

  auto json = svp::query::traversal_result_to_json(result);
  assert(json.contains("graph_health"));
  assert(json["graph_health"].contains("total_edges"));
  assert(json["graph_health"].contains("total_nodes"));
  assert(json["graph_health"].contains("class_counts"));

  std::cout << "test_graph_health_report: passed\n";
}

void test_time_window_in_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 1;
  opts.limit = 100;
  opts.time_window.at_us = 500000;

  auto result = svp::query::traverse_relationships(pkg, opts);
  auto json = svp::query::traversal_result_to_json(result);

  assert(json.contains("time_window"));
  assert(json["time_window"].contains("at_us"));
  assert(json["time_window"].value("at_us", 0) == 500000);

  std::cout << "test_time_window_in_json_output: passed\n";
}

void test_deterministic_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result1 = svp::query::traverse_relationships(pkg, opts);
  auto result2 = svp::query::traverse_relationships(pkg, opts);

  auto json1 = svp::query::traversal_result_to_json(result1);
  auto json2 = svp::query::traversal_result_to_json(result2);

  assert(json1.dump() == json2.dump());

  std::cout << "test_deterministic_json_output: passed\n";
}

void test_semantic_relationships_generated() {
  const auto pkg = create_semantic_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_appears_in_shot = false;
  bool found_appears_in_scene = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "appears_in_shot") found_appears_in_shot = true;
    if (edge.relationship_type == "appears_in_scene") found_appears_in_scene = true;
  }
  assert(found_appears_in_shot);
  assert(found_appears_in_scene);

  std::cout << "test_semantic_relationships_generated: passed\n";
}

void test_semantic_visible_during_speech() {
  const auto pkg = create_semantic_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "text_region_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_visible_during_speech = false;
  bool found_visible_during_word_range = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "visible_during_speech") found_visible_during_speech = true;
    if (edge.relationship_type == "visible_during_word_range") found_visible_during_word_range = true;
  }
  assert(found_visible_during_speech);
  assert(found_visible_during_word_range);

  std::cout << "test_semantic_visible_during_speech: passed\n";
}

void test_semantic_speaker_active_during_entity_visible() {
  const auto pkg = create_semantic_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "seg_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_speaker_active = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "speaker_active_during_entity_visible") {
      found_speaker_active = true;
    }
  }
  assert(found_speaker_active);

  std::cout << "test_semantic_speaker_active_during_entity_visible: passed\n";
}

void test_unresolved_ids_in_graph_health() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  bool has_unresolved = false;
  for (const auto& node : result.nodes) {
    if (!node.resolved) {
      has_unresolved = true;
      break;
    }
  }

  if (has_unresolved) {
    assert(!result.missing_object_ids.empty());
  }

  auto json = svp::query::traversal_result_to_json(result);
  if (json.contains("missing_object_ids")) {
    assert(json["missing_object_ids"].is_array());
  }

  std::cout << "test_unresolved_ids_in_graph_health: passed\n";
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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}

void test_zero_pts_semantic_relationships() {
  const auto pkg = create_zero_pts_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_appears_in_shot = false;
  bool found_appears_in_scene = false;
  bool found_visible_during_speech = false;
  bool found_speaker_active = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "appears_in_shot") found_appears_in_shot = true;
    if (edge.relationship_type == "appears_in_scene") found_appears_in_scene = true;
    if (edge.relationship_type == "visible_during_speech") found_visible_during_speech = true;
    if (edge.relationship_type == "speaker_active_during_entity_visible") found_speaker_active = true;
  }
  assert(found_appears_in_shot);
  assert(found_appears_in_scene);
  assert(found_visible_during_speech);
  assert(found_speaker_active);

  std::cout << "test_zero_pts_semantic_relationships: passed\n";
}

std::filesystem::path create_frame_connectivity_test_package() {
  const auto root = std::filesystem::temp_directory_path() / "svp-query-frame-conn-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto package_path = root / "test_frame_conn.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_frame_conn_test_pkg"},
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
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}},
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

  // Frames with both shot_id and scene_id — the key test data
  write_jsonl(staging / "timeline" / "frames.jsonl", {
      {{"id", "frame_000001"}, {"pts_us", 0}, {"shot_id", "shot_000001"}, {"scene_id", "scene_000001"}},
      {{"id", "frame_000002"}, {"pts_us", 50000}, {"shot_id", "shot_000001"}, {"scene_id", "scene_000001"}},
      {{"id", "frame_000003"}, {"pts_us", 100000}, {"shot_id", "shot_000002"}, {"scene_id", "scene_000001"}}
  });
  write_jsonl(staging / "timeline" / "shots.jsonl", {
      {{"id", "shot_000001"}, {"start_us", 0}, {"end_us", 10000000}},
      {{"id", "shot_000002"}, {"start_us", 10000000}, {"end_us", 20000000}}
  });
  write_jsonl(staging / "timeline" / "scenes.jsonl", {
      {{"id", "scene_000001"}, {"start_us", 0}, {"end_us", 30000000},
       {"shot_ids", {"shot_000001", "shot_000002"}}}
  });

  write_jsonl(staging / "entities" / "entities.jsonl", {});
  write_jsonl(staging / "entities" / "entity_tracks.jsonl", {});

  std::filesystem::create_directories(staging / "spatial");
  write_file(staging / "spatial" / "regions.jsonl", "");
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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}

void test_frame_shot_scene_connectivity() {
  const auto pkg = create_frame_connectivity_test_package();

  // Traverse from frame_000001 — should reach shot_000001 and scene_000001
  svp::query::TraversalOptions opts;
  opts.start_id = "frame_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_frame_in_shot = false;
  bool found_frame_in_scene = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "frame_in_shot" && edge.target_id == "shot_000001") {
      found_frame_in_shot = true;
    }
    if (edge.relationship_type == "frame_in_scene" && edge.target_id == "scene_000001") {
      found_frame_in_scene = true;
    }
  }
  assert(found_frame_in_shot);
  assert(found_frame_in_scene);

  // Verify shot_000001 and scene_000001 are in visited nodes
  std::set<std::string> visited_ids;
  for (const auto& node : result.nodes) {
    visited_ids.insert(node.object_id);
  }
  assert(visited_ids.count("shot_000001") > 0);
  assert(visited_ids.count("scene_000001") > 0);

  std::cout << "test_frame_shot_scene_connectivity: passed\n";
}

void test_frame_connectivity_all_frames() {
  const auto pkg = create_frame_connectivity_test_package();

  // Verify every frame has at least one outgoing edge to shot or scene
  const std::string frame_ids[] = {"frame_000001", "frame_000002", "frame_000003"};
  for (const auto& frame_id : frame_ids) {
    svp::query::TraversalOptions opts;
    opts.start_id = frame_id;
    opts.max_depth = 1;
    opts.direction = svp::query::TraversalDirection::Outgoing;
    opts.limit = 100;

    auto result = svp::query::traverse_relationships(pkg, opts);
    assert(result.error_message.empty());

    bool has_edge = false;
    for (const auto& edge : result.edges) {
      if (edge.relationship_type == "frame_in_shot" || edge.relationship_type == "frame_in_scene") {
        has_edge = true;
        break;
      }
    }
    assert(has_edge);
  }

  std::cout << "test_frame_connectivity_all_frames: passed\n";
}

void test_frame_traversal_to_scene() {
  const auto pkg = create_frame_connectivity_test_package();

  // Traverse from frame_000003 (in shot_000002, scene_000001)
  // Verify it reaches scene_000001 via frame_in_scene
  svp::query::TraversalOptions opts;
  opts.start_id = "frame_000003";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.type_filter = "frame_in_scene";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_scene = false;
  for (const auto& edge : result.edges) {
    assert(edge.relationship_type == "frame_in_scene");
    if (edge.target_id == "scene_000001") {
      found_scene = true;
    }
  }
  assert(found_scene);

  std::cout << "test_frame_traversal_to_scene: passed\n";
}

void test_frame_in_shot_classified_as_support() {
  const auto pkg = create_frame_connectivity_test_package();

  // Filter by support class — frame_in_shot and frame_in_scene should appear
  svp::query::TraversalOptions opts;
  opts.start_id = "frame_000001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "support";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "frame_in_shot" || edge.relationship_type == "frame_in_scene") {
      found = true;
      break;
    }
  }
  assert(found);

  std::cout << "test_frame_in_shot_classified_as_support: passed\n";
}

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
       {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}}
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

  bool ok = svp::package::write_package_skeleton(package_path, staging, source_path, manifest);
  assert(ok);
  assert(std::filesystem::exists(package_path));

  return package_path;
}

void test_spatial_overlaps_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "region_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_overlaps = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "overlaps") {
      found_overlaps = true;
      break;
    }
  }
  assert(found_overlaps);

  std::cout << "test_spatial_overlaps_relationship: passed\n";
}

void test_spatial_contains_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "region_000004";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_contains = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "contains") {
      found_contains = true;
      break;
    }
  }
  assert(found_contains);

  std::cout << "test_spatial_contains_relationship: passed\n";
}

void test_spatial_near_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "region_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_near = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "near") {
      found_near = true;
      break;
    }
  }
  assert(found_near);

  std::cout << "test_spatial_near_relationship: passed\n";
}

void test_entity_enters_frame_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_enters = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "enters_frame" && edge.target_id == "frame_000001") {
      found_enters = true;
      break;
    }
  }
  assert(found_enters);

  std::cout << "test_entity_enters_frame_relationship: passed\n";
}

void test_entity_exits_frame_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_exits = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "exits_frame" && edge.target_id == "frame_000003") {
      found_exits = true;
      break;
    }
  }
  assert(found_exits);

  std::cout << "test_entity_exits_frame_relationship: passed\n";
}

void test_context_with_depth() {
  const auto pkg = create_traversal_test_package();

  svp::query::ContextOptions opts;
  opts.object_id = "word_000001";
  opts.max_depth = 2;
  opts.limit = 100;

  auto result = svp::query::build_context(pkg, opts);

  assert(result.object_id == "word_000001");
  assert(result.resolved);
  assert(!result.context_edges.empty());

  bool found_speaker = false;
  for (const auto& node : result.context_nodes) {
    if (node.object_id == "speaker_0001") found_speaker = true;
  }
  assert(found_speaker);

  std::cout << "test_context_with_depth: passed\n";
}

void test_context_with_time_window() {
  const auto pkg = create_traversal_test_package();

  svp::query::ContextOptions opts;
  opts.object_id = "word_000001";
  opts.max_depth = 2;
  opts.limit = 100;
  opts.at_us = 200000;

  auto result = svp::query::build_context(pkg, opts);

  assert(result.object_id == "word_000001");
  assert(result.resolved);

  for (const auto& edge : result.context_edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start <= 200000 && 200000 < end);
  }

  std::cout << "test_context_with_time_window: passed\n";
}

void test_context_at_and_start_mutually_exclusive() {
  const auto pkg = create_traversal_test_package();

  svp::query::ContextOptions opts;
  opts.object_id = "word_000001";
  opts.at_us = 100;
  opts.start_us = 0;

  auto result = svp::query::build_context(pkg, opts);

  assert(!result.error_message.empty());
  assert(result.error_message.find("cannot specify both") != std::string::npos);

  std::cout << "test_context_at_and_start_mutually_exclusive: passed\n";
}

void test_graph_health_diagnostics() {
  const auto pkg = create_traversal_test_package();

  auto health = svp::query::compute_graph_health(pkg);

  assert(health.total_edges > 0);
  assert(health.total_nodes > 0);
  assert(health.resolved_nodes > 0);
  assert(!health.class_counts.empty());
  assert(!health.type_counts.empty());

  auto json = svp::query::graph_health_to_json(health);
  assert(json.contains("total_edges"));
  assert(json.contains("total_nodes"));
  assert(json.contains("resolved_nodes"));
  assert(json.contains("unresolved_nodes"));
  assert(json.contains("orphan_nodes"));
  assert(json.contains("class_counts"));
  assert(json.contains("type_counts"));

  std::cout << "test_graph_health_diagnostics: passed\n";
}

void test_graph_health_diagnostics_json_deterministic() {
  const auto pkg = create_traversal_test_package();

  auto health1 = svp::query::compute_graph_health(pkg);
  auto health2 = svp::query::compute_graph_health(pkg);

  auto json1 = svp::query::graph_health_to_json(health1);
  auto json2 = svp::query::graph_health_to_json(health2);

  assert(json1.dump() == json2.dump());

  std::cout << "test_graph_health_diagnostics_json_deterministic: passed\n";
}

void test_graph_health_enhanced_diagnostics() {
  const auto pkg = create_test_package();

  auto health = svp::query::compute_graph_health(pkg);
  auto j = svp::query::graph_health_to_json(health);

  // Verify new fields are present in JSON output.
  assert(j.contains("backend_used"));
  assert(j.contains("index_available"));
  assert(j.contains("mask_data_available"));
  assert(j.contains("depth_data_available"));

  // The test package has no mask or depth data, so skipped categories should
  // report them honestly.
  assert(j.contains("skipped_categories"));
  const auto& skipped = j["skipped_categories"];
  assert(skipped.is_array());
  assert(skipped.size() >= 2);

  bool found_mask_skip = false;
  bool found_depth_skip = false;
  bool found_motion_skip = false;
  for (const auto& cat : skipped) {
    const std::string category = cat["category"];
    const std::string reason = cat["reason"];
    if (category.find("occludes") != std::string::npos) found_mask_skip = true;
    if (category.find("foreground") != std::string::npos) found_depth_skip = true;
    if (category.find("moves_with") != std::string::npos) found_motion_skip = true;
    assert(!reason.empty());
  }
  assert(found_mask_skip);
  assert(found_depth_skip);
  assert(found_motion_skip);

  // Backend should be jsonl (no index.sqlite in test package).
  assert(j["backend_used"] == "jsonl");
  assert(j["index_available"] == false);

  std::cout << "test_graph_health_enhanced_diagnostics: passed\n";
}

void test_traversal_jsonl_sqlite_parity() {
  // The traversal test package (create_traversal_test_package) includes
  // relationships with known edges. Since the test package doesn't have
  // index.sqlite, both paths use JSONL. This test verifies that the
  // traversal result is consistent and the backend is correctly reported.
  const auto pkg = create_traversal_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  // The package should have edges from word_000001 to speaker_0001
  // and other support relationships.
  assert(result.error_message.empty());
  assert(!result.edges.empty());
  assert(result.visited_node_count > 1);

  // Graph health should report the JSONL backend since no index.sqlite.
  auto health = svp::query::compute_graph_health(pkg);
  assert(health.backend_used == svp::query::TraversalBackend::jsonl);
  assert(!health.index_available);
  assert(health.total_edges > 0);

  std::cout << "test_traversal_jsonl_sqlite_parity: passed\n";
}

}  // namespace

int main() {
  test_read_jsonl_entry();
  test_read_json_entry();
  test_list_layers();
  test_transcript_summary();
  test_find_words();
  test_list_speakers();
  test_list_ocr_observations();
  test_list_color_observations();
  test_show_validation();
  test_malformed_jsonl();
  test_malformed_transcript_json();
  test_malformed_validation_json();
  test_malformed_layers();
  test_relationship_summary();
  test_list_relationships_all();
  test_list_relationships_filtered_by_support();
  test_list_relationships_filtered_by_semantic();
  test_list_relationships_filtered_by_unknown();
  test_json_type_field_canonical_in_query();
  test_traversal_outgoing();
  test_traversal_incoming();
  test_traversal_both_directions();
  test_traversal_max_depth();
  test_traversal_cycle_safety();
  test_traversal_class_filter();
  test_traversal_type_filter();
  test_traversal_unknown_type_traversable();
  test_traversal_missing_ids_reported();
  test_traversal_embedding_id_resolves();
  test_object_catalog_annotations();
  test_traversal_json_output();
  test_traversal_empty_relationships();
  test_traversal_missing_relationships();
  test_traversal_malformed_relationships();
  test_time_window_at_us();
  test_time_window_start_end();
  test_time_window_at_us_excludes_non_overlapping();
  test_time_window_at_and_start_mutually_exclusive();
  test_shortest_path_found();
  test_shortest_path_not_found();
  test_shortest_path_same_node();
  test_shortest_path_json_output();
  test_context_mode();
  test_context_json_output();
  test_context_unresolved_object();
  test_edge_deduplication();
  test_graph_health_report();
  test_time_window_in_json_output();
  test_deterministic_json_output();
  test_semantic_relationships_generated();
  test_semantic_visible_during_speech();
  test_semantic_speaker_active_during_entity_visible();
  test_unresolved_ids_in_graph_health();
  test_zero_pts_semantic_relationships();
  test_frame_shot_scene_connectivity();
  test_frame_connectivity_all_frames();
  test_frame_traversal_to_scene();
  test_frame_in_shot_classified_as_support();
  test_spatial_overlaps_relationship();
  test_spatial_contains_relationship();
  test_spatial_near_relationship();
  test_entity_enters_frame_relationship();
  test_entity_exits_frame_relationship();
  test_context_with_depth();
  test_context_with_time_window();
  test_context_at_and_start_mutually_exclusive();
  test_graph_health_diagnostics();
  test_graph_health_diagnostics_json_deterministic();
  test_graph_health_enhanced_diagnostics();
  test_traversal_jsonl_sqlite_parity();

  std::cout << "All svp-query tests passed.\n";
  return 0;
}
