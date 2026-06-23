#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/entity_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"
#include "svp/package/spatial_embedding_placeholders.hpp"
#include "svp/package/timeline_writer.hpp"
#include "svp/package/validation_report_storage.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/foundation_color_staging.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<nlohmann::json> read_jsonl_records(const std::filesystem::path& path) {
  std::vector<nlohmann::json> records;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      records.push_back(nlohmann::json::parse(line));
    }
  }
  return records;
}

void test_write_package_skeleton_creates_atomic_zip_file() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-package-writer-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path package_path = root / "output.svp";
  const std::filesystem::path staging_dir = root / "staging";
  const std::filesystem::path source_path = root / "input.mov";

  // Create staging directory and a file
  std::filesystem::create_directories(staging_dir / "text");
  {
    std::ofstream out(staging_dir / "text" / "text_absence.json");
    out << "{\"reason\": \"test\"}\n";
  }

  // Create mock source file
  {
    std::ofstream out(source_path);
    out << "mock media content\n";
  }

  // Define mock manifest
  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"},
    {"package_id", "svp_test_pkg_id"},
    {"created_utc", "2026-06-20T00:00:00Z"},
    {"primary_media_id", "media_000001"},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }}
  };

  // Run writer
  bool success = svp::package::write_package_skeleton(
      package_path, staging_dir, source_path, manifest);

  assert(success);
  assert(std::filesystem::exists(package_path));
  assert(!std::filesystem::exists(package_path.string() + ".tmp"));

  // Verify ZIP contents using existing package layout reader
  auto layout_result = svp::package::read_package_layout(package_path);
  assert(layout_result.has_value());
  const auto& layout = layout_result.value();

  // Mimetype and manifest should be present
  assert(layout.has_entry("mimetype"));
  assert(layout.has_entry("manifest.json"));

  // Top level sections should be present
  assert(layout.has_top_level_section("media"));
  assert(layout.has_top_level_section("transcript"));
  assert(layout.has_top_level_section("text"));

  // Staged files should be present
  assert(layout.has_entry("text/text_absence.json"));

  // Source file should be present in media/original
  assert(layout.has_entry("media/original/source_000.mov"));

  // Verify mimetype content
  auto mime_content = svp::package::read_package_entry(package_path, "mimetype");
  assert(mime_content.has_value());
  assert(mime_content.value() == "application/vnd.svp+zip");

  // Verify manifest content
  auto manifest_content = svp::package::read_package_entry(package_path, "manifest.json");
  assert(manifest_content.has_value());
  nlohmann::json parsed_manifest = nlohmann::json::parse(manifest_content.value());
  assert(parsed_manifest["svp_version"] == "1.0-rc.2");
  assert(parsed_manifest["package_id"] == "svp_test_pkg_id");

  std::filesystem::remove_all(root);
}

void test_writer_fails_gracefully_on_missing_staging_dir() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-package-writer-fail-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path package_path = root / "output.svp";
  const std::filesystem::path staging_dir = root / "non_existent_staging";
  const std::filesystem::path source_path = "";

  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"}
  };

  // Even if staging_dir doesn't exist, it should still succeed by writing the skeleton directories and mimetype/manifest!
  bool success = svp::package::write_package_skeleton(
      package_path, staging_dir, source_path, manifest);

  assert(success);
  assert(std::filesystem::exists(package_path));

  auto layout_result = svp::package::read_package_layout(package_path);
  assert(layout_result.has_value());
  assert(layout_result.value().has_entry("mimetype"));

  std::filesystem::remove_all(root);
}

void test_writer_fails_on_rename_and_cleans_up_temp() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-package-writer-rename-fail-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path package_path = root / "output.svp";
  std::filesystem::create_directories(package_path);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);
  const std::filesystem::path source_path = "";

  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"}
  };

  bool success = svp::package::write_package_skeleton(
      package_path, staging_dir, source_path, manifest);

  assert(!success);

  const std::filesystem::path temp_path = package_path.string() + ".tmp";
  assert(!std::filesystem::exists(temp_path));

  std::filesystem::remove_all(root);
}

void test_write_relationships_and_provenance() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-relationships-provenance-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "provenance");

  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    nlohmann::json reg = {
      {"text_region_id", "text_region_001"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"shot_id", "shot_001"},
      {"scene_id", "scene_001"},
      {"confidence", 0.75}
    };
    out << reg.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "b"}}.dump() << "\n";
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
    out << nlohmann::json{{"id", "processor_color_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);
  // No timeline files present, so shot_id/scene_id references are skipped.
  assert(summary.relationships_written == 0);
  assert(summary.type_counts.text_region_shot == 0);
  assert(summary.type_counts.text_region_scene == 0);
  assert(summary.type_counts.skipped_dangling == 2);
  // No relationship processor added when no relationships.
  // The existing processors should still be there (deduped).
  assert(summary.processors_written == 2);
  assert(summary.duplicate_processors_merged == 1);

  const std::vector<nlohmann::json> relationships =
      read_jsonl_records(staging_dir / "relationships" / "relationships.jsonl");
  assert(relationships.empty());

  const std::vector<nlohmann::json> processors =
      read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  assert(processors.size() == 2);
  assert(processors[0]["id"] == "processor_color_0001");
  assert(processors[1]["id"] == "processor_ocr_0001");
  assert(processors[1]["version"] == "a");

  std::filesystem::remove_all(root);
}

void test_full_relationship_graph() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-full-relationship-graph-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "transcript");
  std::filesystem::create_directories(staging_dir / "colors");
  std::filesystem::create_directories(staging_dir / "spatial");
  std::filesystem::create_directories(staging_dir / "embeddings");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Timeline artifacts — canonical owners of frame, shot, scene IDs
  {
    std::ofstream out(staging_dir / "timeline" / "frames.jsonl");
    out << nlohmann::json{{"id", "frame_000001"}, {"start_us", 0}, {"end_us", 33333}}.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json{{"id", "shot_001"}, {"start_us", 0}, {"end_us", 10000000}}.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "scenes.jsonl");
    out << nlohmann::json{{"id", "scene_001"}, {"start_us", 0}, {"end_us", 30000000}}.dump() << "\n";
  }

  // Text region with shot and scene
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json{
      {"text_region_id", "tr_001"},
      {"start_us", 1000},
      {"end_us", 5000},
      {"shot_id", "shot_001"},
      {"scene_id", "scene_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  // Text observation linked to region, with evidence crop ref
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json{
      {"text_observation_id", "obs_001"},
      {"text_region_id", "tr_001"},
      {"raw_text", "Hello"},
      {"normalized_text", "hello"},
      {"confidence", 0.85},
      {"evidence_crop_refs", nlohmann::json::array({"crop_001"})}
    }.dump() << "\n";
  }

  // Evidence crop
  {
    std::ofstream out(staging_dir / "text" / "evidence_crops.jsonl");
    out << nlohmann::json{
      {"crop_id", "crop_001"},
      {"text_region_id", "tr_001"},
      {"text_observation_id", "obs_001"}
    }.dump() << "\n";
  }

  // Numeric value linked to observation
  {
    std::ofstream out(staging_dir / "text" / "numeric_values.jsonl");
    out << nlohmann::json{
      {"numeric_value_id", "num_001"},
      {"text_observation_id", "obs_001"},
      {"text_region_id", "tr_001"},
      {"numeric_value", "42"},
      {"raw_text", "42"},
      {"normalized_text", "42"},
      {"confidence", 0.95}
    }.dump() << "\n";
  }

  // Words with speaker_id
  {
    std::ofstream out(staging_dir / "transcript" / "words.jsonl");
    out << nlohmann::json{
      {"id", "word_001"},
      {"text", "hello"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"speaker_id", "speaker_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
    out << nlohmann::json{
      {"id", "word_002"},
      {"text", "world"},
      {"start_us", 2000},
      {"end_us", 3000},
      {"speaker_id", "speaker_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  // Speakers
  {
    std::ofstream out(staging_dir / "transcript" / "speakers.jsonl");
    out << nlohmann::json{
      {"id", "speaker_001"},
      {"display_name", "Speaker 1"},
      {"confidence", 0.8}
    }.dump() << "\n";
  }

  // Speaker segments
  {
    std::ofstream out(staging_dir / "transcript" / "speaker_segments.jsonl");
    out << nlohmann::json{
      {"id", "seg_001"},
      {"speaker_id", "speaker_001"},
      {"start_us", 0},
      {"end_us", 5000},
      {"confidence", 0.85}
    }.dump() << "\n";
  }

  // Color observation targeting a frame
  {
    std::ofstream out(staging_dir / "colors" / "color_observations.jsonl");
    out << nlohmann::json{
      {"color_observation_id", "color_001"},
      {"target_type", "frame"},
      {"target_id", "frame_000001"},
      {"dominant_bucket", "orange"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  // Depth index entry with frame_id
  {
    std::ofstream out(staging_dir / "spatial" / "depth.index.jsonl");
    out << nlohmann::json{
      {"id", "depth_000001"},
      {"frame_id", "frame_000001"},
      {"start_us", 0},
      {"end_us", 0}
    }.dump() << "\n";
  }

  // Embedding index entry with input_ref to text_observation
  {
    std::ofstream out(staging_dir / "embeddings" / "embeddings.index.jsonl");
    out << nlohmann::json{
      {"id", "embed_obs_001"},
      {"embedding_set_id", "embedset_text_nomic_v15"},
      {"input_ref", "obs_001"},
      {"input_kind", "text_observation"}
    }.dump() << "\n";
  }

  // Processor provenance
  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  // Expected relationships:
  // 1. tr_001 -> shot_001 (appears_in_shot)
  // 2. tr_001 -> scene_001 (appears_in_scene)
  // 3. obs_001 -> tr_001 (observation_in_region)
  // 4. obs_001 -> crop_001 (has_evidence_crop)
  // 5. num_001 -> obs_001 (numeric_value_from_observation)
  // 6. word_001 -> speaker_001 (word_spoken_by)
  // 7. word_001 -> seg_001 (word_in_speaker_segment)
  // 8. word_002 -> speaker_001 (word_spoken_by)
  // 9. word_002 -> seg_001 (word_in_speaker_segment)
  // 10. color_001 -> frame_000001 (color_observation_of)
  // 11. depth_000001 -> frame_000001 (depth_for_frame)
  // 12. embed_obs_001 -> obs_001 (embedding_source_is)
  assert(summary.relationships_written == 12);
  assert(summary.type_counts.text_region_shot == 1);
  assert(summary.type_counts.text_region_scene == 1);
  assert(summary.type_counts.text_observation_region == 1);
  assert(summary.type_counts.text_observation_evidence_crop == 1);
  assert(summary.type_counts.numeric_value_observation == 1);
  assert(summary.type_counts.word_speaker == 2);
  assert(summary.type_counts.word_speaker_segment == 2);
  assert(summary.type_counts.word_speaker_segment_unmatched == 0);
  assert(summary.type_counts.color_observation_target == 1);
  assert(summary.type_counts.depth_frame == 1);
  assert(summary.type_counts.embedding_source == 1);
  assert(summary.type_counts.skipped_dangling == 0);

  // Verify processor provenance includes type counts
  const auto processors =
      read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  bool found_rel_processor = false;
  for (const auto& proc : processors) {
    if (proc["id"] == "processor_relationship_writer_0001") {
      found_rel_processor = true;
      assert(proc.contains("relationship_type_counts"));
      assert(proc["relationship_type_counts"]["text_region_shot"] == 1);
      assert(proc["relationship_type_counts"]["skipped_dangling"] == 0);
      assert(proc["relationship_type_counts"]["word_speaker_segment_unmatched"] == 0);
    }
  }
  assert(found_rel_processor);

  // Verify no dangling references: every source_id and target_id
  // must exist in the known ID sets.
  const auto relationships =
      read_jsonl_records(staging_dir / "relationships" / "relationships.jsonl");
  std::set<std::string> all_known_ids = {
    "tr_001", "shot_001", "scene_001",
    "obs_001", "crop_001", "num_001",
    "word_001", "word_002", "speaker_001", "seg_001",
    "color_001", "frame_000001",
    "depth_000001", "embed_obs_001"
  };
  for (const auto& rel : relationships) {
    const std::string src = rel["source_id"].get<std::string>();
    const std::string tgt = rel["target_id"].get<std::string>();
    assert(all_known_ids.count(src) > 0);
    assert(all_known_ids.count(tgt) > 0);
  }

  std::filesystem::remove_all(root);
}

void test_depth_index_alone_no_frame_ids() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-depth-alone-no-frame-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "spatial");
  std::filesystem::create_directories(staging_dir / "colors");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Depth index with frame_id, but no timeline/frames.jsonl
  {
    std::ofstream out(staging_dir / "spatial" / "depth.index.jsonl");
    out << nlohmann::json{
      {"id", "depth_000001"},
      {"frame_id", "frame_000001"},
      {"start_us", 0},
      {"end_us", 33333}
    }.dump() << "\n";
  }

  // Color observation targeting same frame_id
  {
    std::ofstream out(staging_dir / "colors" / "color_observations.jsonl");
    out << nlohmann::json{
      {"color_observation_id", "color_001"},
      {"target_type", "frame"},
      {"target_id", "frame_000001"},
      {"dominant_bucket", "orange"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  // depth_for_frame must NOT be emitted because frame_000001
  // is only known from depth.index, not from timeline/frames.jsonl.
  assert(summary.type_counts.depth_frame == 0);
  // color_observation_of to frame must NOT be emitted for same reason.
  assert(summary.type_counts.color_observation_target == 0);
  assert(summary.relationships_written == 0);
  assert(summary.type_counts.skipped_dangling >= 2);

  std::filesystem::remove_all(root);
}

void test_color_frame_target_skipped_without_timeline() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-color-skip-no-timeline-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "colors");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Color observation targeting a frame, but no timeline/frames.jsonl
  {
    std::ofstream out(staging_dir / "colors" / "color_observations.jsonl");
    out << nlohmann::json{
      {"color_observation_id", "color_001"},
      {"target_type", "frame"},
      {"target_id", "frame_999"},
      {"dominant_bucket", "blue"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  assert(summary.type_counts.color_observation_target == 0);
  assert(summary.relationships_written == 0);
  assert(summary.type_counts.skipped_dangling >= 1);

  std::filesystem::remove_all(root);
}

void test_text_region_shot_scene_skipped_without_timeline() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-tr-shot-skip-no-timeline-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "provenance");

  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json{
      {"text_region_id", "tr_001"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"shot_id", "shot_001"},
      {"scene_id", "scene_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  // shot_id and scene_id from text_regions must NOT be treated as known
  // because timeline/shots.jsonl and timeline/scenes.jsonl are absent.
  assert(summary.type_counts.text_region_shot == 0);
  assert(summary.type_counts.text_region_scene == 0);
  assert(summary.relationships_written == 0);
  assert(summary.type_counts.skipped_dangling == 2);

  std::filesystem::remove_all(root);
}

void test_timeline_present_enables_relationships() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-timeline-present-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "colors");
  std::filesystem::create_directories(staging_dir / "spatial");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Timeline files
  {
    std::ofstream out(staging_dir / "timeline" / "frames.jsonl");
    out << nlohmann::json{{"id", "frame_001"}, {"start_us", 0}, {"end_us", 33333}}.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json{{"id", "shot_001"}, {"start_us", 0}, {"end_us", 10000000}}.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "scenes.jsonl");
    out << nlohmann::json{{"id", "scene_001"}, {"start_us", 0}, {"end_us", 30000000}}.dump() << "\n";
  }

  // Text region referencing shot and scene
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json{
      {"text_region_id", "tr_001"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"shot_id", "shot_001"},
      {"scene_id", "scene_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  // Color observation targeting frame
  {
    std::ofstream out(staging_dir / "colors" / "color_observations.jsonl");
    out << nlohmann::json{
      {"color_observation_id", "color_001"},
      {"target_type", "frame"},
      {"target_id", "frame_001"},
      {"dominant_bucket", "orange"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  // Depth index targeting frame
  {
    std::ofstream out(staging_dir / "spatial" / "depth.index.jsonl");
    out << nlohmann::json{
      {"id", "depth_001"},
      {"frame_id", "frame_001"},
      {"start_us", 0},
      {"end_us", 33333}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  // With timeline files present, these relationships should now be emitted.
  assert(summary.type_counts.text_region_shot == 1);
  assert(summary.type_counts.text_region_scene == 1);
  assert(summary.type_counts.color_observation_target == 1);
  assert(summary.type_counts.depth_frame == 1);
  assert(summary.relationships_written == 4);
  assert(summary.type_counts.skipped_dangling == 0);

  std::filesystem::remove_all(root);
}

void test_unmatched_word_segment_counting() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-unmatched-word-seg-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "transcript");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Two words: word_001 overlaps segment, word_002 does not
  {
    std::ofstream out(staging_dir / "transcript" / "words.jsonl");
    out << nlohmann::json{
      {"id", "word_001"},
      {"text", "hello"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"speaker_id", "speaker_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
    out << nlohmann::json{
      {"id", "word_002"},
      {"text", "world"},
      {"start_us", 20000},
      {"end_us", 21000},
      {"speaker_id", "speaker_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "transcript" / "speakers.jsonl");
    out << nlohmann::json{
      {"id", "speaker_001"},
      {"display_name", "Speaker 1"},
      {"confidence", 0.8}
    }.dump() << "\n";
  }

  // Speaker segment covers 0-5000us, word_002 is at 20000us (no overlap)
  {
    std::ofstream out(staging_dir / "transcript" / "speaker_segments.jsonl");
    out << nlohmann::json{
      {"id", "seg_001"},
      {"speaker_id", "speaker_001"},
      {"start_us", 0},
      {"end_us", 5000},
      {"confidence", 0.85}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  assert(summary.type_counts.word_speaker == 2);
  assert(summary.type_counts.word_speaker_segment == 1);
  assert(summary.type_counts.word_speaker_segment_unmatched == 1);

  std::filesystem::remove_all(root);
}

void test_relationships_with_missing_sections() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-relationships-missing-sections-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "provenance");

  // No text regions, no transcript, no colors, no depth, no embeddings
  // Only a processor record
  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  assert(summary.relationships_written == 0);
  assert(summary.type_counts.skipped_dangling == 0);
  // No relationship processor added when no relationships
  // The existing processor should still be there
  const auto processors =
      read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  assert(processors.size() == 1);
  assert(processors[0]["id"] == "processor_ocr_0001");

  // relationships.jsonl should exist but be empty
  const auto relationships =
      read_jsonl_records(staging_dir / "relationships" / "relationships.jsonl");
  assert(relationships.empty());

  std::filesystem::remove_all(root);
}

void test_relationships_dangling_reference_prevention() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-relationships-dangling-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "transcript");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Text region with shot_id that doesn't exist as a separate entity
  // (shot_id is just a string in the text_region, not a registered object)
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json{
      {"text_region_id", "tr_001"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"shot_id", "shot_001"},
      {"scene_id", "scene_001"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  // Text observation with a dangling evidence_crop_ref (crop doesn't exist)
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json{
      {"text_observation_id", "obs_001"},
      {"text_region_id", "tr_001"},
      {"raw_text", "Hello"},
      {"normalized_text", "hello"},
      {"confidence", 0.85},
      {"evidence_crop_refs", nlohmann::json::array({"crop_nonexistent"})}
    }.dump() << "\n";
  }

  // Word with a dangling speaker_id (speaker doesn't exist)
  {
    std::ofstream out(staging_dir / "transcript" / "words.jsonl");
    out << nlohmann::json{
      {"id", "word_001"},
      {"text", "hello"},
      {"start_us", 1000},
      {"end_us", 2000},
      {"speaker_id", "speaker_nonexistent"},
      {"confidence", 0.9}
    }.dump() << "\n";
  }

  {
    std::ofstream out(staging_dir / "provenance" / "processors.jsonl");
    out << nlohmann::json{{"id", "processor_ocr_0001"}, {"version", "a"}}.dump() << "\n";
  }

  const svp::package::RelationshipProvenanceWriteSummary summary =
      svp::package::write_relationships_and_provenance(staging_dir);

  // No timeline files present, so shot_id/scene_id from text_regions
  // are not known IDs and should be skipped.
  // obs_001 -> tr_001 is valid (tr_001 exists in text_regions).
  // obs_001 -> crop_nonexistent should be skipped (dangling).
  // word_001 -> speaker_nonexistent should be skipped (dangling).
  assert(summary.type_counts.text_region_shot == 0);
  assert(summary.type_counts.text_region_scene == 0);
  assert(summary.type_counts.text_observation_region == 1);
  assert(summary.type_counts.text_observation_evidence_crop == 0);
  assert(summary.type_counts.word_speaker == 0);
  assert(summary.type_counts.skipped_dangling >= 4);

  std::filesystem::remove_all(root);
}

void test_write_index_foundation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-index-writer-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);

  // Staging color observation
  std::filesystem::create_directories(staging_dir / "colors");
  {
    std::ofstream out(staging_dir / "colors" / "color_observations.jsonl");
    nlohmann::json col = {
      {"color_observation_id", "color_001"},
      {"target_type", "frame"},
      {"target_id", "frame_000001"},
      {"dominant_bucket", "orange"},
      {"bucket_coverage", {{"orange", 0.85}, {"black", 0.15}}}
    };
    out << col.dump() << "\n";
  }

  // Staging text records
  std::filesystem::create_directories(staging_dir / "text");
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    nlohmann::json reg = {
      {"text_region_id", "region_001"},
      {"start_us", 0},
      {"end_us", 1000000},
      {"shot_id", "shot_000001"},
      {"scene_id", "scene_000001"}
    };
    out << reg.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    nlohmann::json obs = {
      {"text_observation_id", "obs_001"},
      {"text_region_id", "region_001"},
      {"raw_text", "SVP"},
      {"normalized_text", "svp"},
      {"layout_class", "title"}
    };
    out << obs.dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "numeric_values.jsonl");
    nlohmann::json num = {
      {"numeric_value_id", "num_001"},
      {"text_observation_id", "obs_001"},
      {"text_region_id", "region_001"},
      {"numeric_value", "1.0"},
      {"raw_text", "1.0"},
      {"normalized_text", "1.0"}
    };
    out << num.dump() << "\n";
  }

  // Staging embedding artifacts
  std::filesystem::create_directories(staging_dir / "embeddings");
  {
    nlohmann::json sets_json = {
        {"sets", nlohmann::json::array({
            {
                {"id", "embedset_text_nomic_v15"},
                {"modality", "text"},
                {"model_id", "model_nomic_embed_text_v1_5"},
                {"model_blake3", "abc123"},
                {"dimension", 768},
                {"dtype", "float32"},
                {"normalized", true},
                {"source_slug", "nomic-ai/nomic-embed-text-v1.5"}
            }
        })}
    };
    std::ofstream out(staging_dir / "embeddings" / "embedding_sets.json");
    out << sets_json.dump(2) << "\n";
  }
  {
    std::ofstream out(staging_dir / "embeddings" / "embeddings.index.jsonl");
    nlohmann::json emb1 = {
        {"id", "embed_obs_001"},
        {"embedding_set_id", "embedset_text_nomic_v15"},
        {"input_ref", "region_001"},
        {"input_kind", "text_observation"},
        {"block_file", "embeddings/embeddings.blocks.svpez"},
        {"block_offset", 0},
        {"block_length", 3200},
        {"payload_offset", 64},
        {"uncompressed_size", 3072},
        {"compressed_size", 3072},
        {"vector_index", 0},
        {"dimension", 768},
        {"dtype", "float32"},
        {"payload_blake3", "deadbeef"},
        {"block_blake3", "cafebabe"}
    };
    nlohmann::json emb2 = {
        {"id", "embed_obs_002"},
        {"embedding_set_id", "embedset_text_nomic_v15"},
        {"input_ref", "region_001"},
        {"input_kind", "text_observation"},
        {"block_file", "embeddings/embeddings.blocks.svpez"},
        {"block_offset", 3200},
        {"block_length", 3200},
        {"payload_offset", 64},
        {"uncompressed_size", 3072},
        {"compressed_size", 3072},
        {"vector_index", 1},
        {"dimension", 768},
        {"dtype", "float32"},
        {"payload_blake3", "deadbee2"},
        {"block_blake3", "cafebab2"}
    };
    out << emb1.dump() << "\n";
    out << emb2.dump() << "\n";
  }

  nlohmann::json manifest = {
    {"package_id", "svp_test_pkg_id"},
    {"created_utc", "2026-06-20T00:00:00Z"}
  };

  const svp::package::RelationshipProvenanceWriteSummary relationship_summary =
      svp::package::write_relationships_and_provenance(staging_dir);
  // No timeline files: shot/scene/color-frame/depth-frame relationships skipped.
  // Expected: obs_001->region_001, num_001->obs_001, 2x embedding_source (region_001 exists)
  assert(relationship_summary.relationships_written == 4);

  bool success = svp::package::write_index_foundation(staging_dir, manifest);
  assert(success);

  // Check that index files exist
  assert(std::filesystem::exists(staging_dir / "index" / "index.sqlite"));
  assert(std::filesystem::exists(staging_dir / "index" / "index_manifest.json"));

  // Check manifest content
  std::ifstream manifest_in(staging_dir / "index" / "index_manifest.json");
  nlohmann::json index_manifest;
  manifest_in >> index_manifest;
  assert(index_manifest["schema_version"] == "svp-index-manifest-v1");
  assert(index_manifest["table_count"] == 15);
  assert(index_manifest["row_count"] > 0);
  assert(index_manifest["created_from"]["manifest_blake3"].get<std::string>().find("blake3:") == 0);

  // Verify embedding_sets_blake3 is NOT the empty-data hash
  const std::string empty_blake3 =
      "blake3:af1349b9f5f9a1a6a040414f808c47f942896582987abaaae1f0110de8e34890";
  const std::string emb_sets_hash =
      index_manifest["created_from"]["embedding_sets_blake3"].get<std::string>();
  assert(emb_sets_hash.find("blake3:") == 0);
  assert(emb_sets_hash != empty_blake3);

  // Open SQLite and verify vector_index has 2 rows
  {
    sqlite3* raw_db = nullptr;
    const auto sqlite_path = staging_dir / "index" / "index.sqlite";
    assert(sqlite3_open_v2(sqlite_path.string().c_str(), &raw_db,
                           SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    assert(raw_db != nullptr);

    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(raw_db,
               "SELECT COUNT(*) FROM vector_index", -1, &stmt, nullptr) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const int vector_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(raw_db);

    assert(vector_count == 2);
  }

  std::filesystem::remove_all(root);
}


void test_spatial_embedding_placeholders() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-spatial-embedding-placeholder-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "provenance");

  const svp::package::SpatialEmbeddingPlaceholderSummary summary =
      svp::package::write_spatial_and_embedding_placeholders(staging_dir, false);

  assert(summary.depth_index_written);
  assert(!summary.depth_blocks_written);
  assert(summary.depth_placeholder_written);
  assert(!summary.depth_generation_run);
  assert(summary.masks_index_written);
  assert(summary.masks_blocks_written);
  assert(summary.embedding_sets_written);
  assert(summary.embeddings_index_written);
  assert(summary.embeddings_blocks_written);
  assert(!summary.embedding_generation_run);
  assert(!summary.model_runtime_available);
  assert(summary.provenance_records_added == 2);

  assert(std::filesystem::exists(staging_dir / "spatial" / "depth.index.jsonl"));
  assert(std::filesystem::exists(staging_dir / "spatial" / "depth.blocks.svpdz"));
  assert(std::filesystem::exists(staging_dir / "spatial" / "masks.index.jsonl"));
  assert(std::filesystem::exists(staging_dir / "spatial" / "masks.blocks.svpmz"));
  assert(std::filesystem::exists(staging_dir / "embeddings" / "embedding_sets.json"));
  assert(std::filesystem::exists(staging_dir / "embeddings" / "embeddings.index.jsonl"));
  assert(std::filesystem::exists(staging_dir / "embeddings" / "embeddings.blocks.svpez"));

  assert(std::filesystem::file_size(staging_dir / "spatial" / "depth.blocks.svpdz") == 0);
  assert(std::filesystem::file_size(staging_dir / "spatial" / "masks.blocks.svpmz") == 0);
  assert(std::filesystem::file_size(staging_dir / "spatial" / "depth.index.jsonl") == 0);
  assert(std::filesystem::file_size(staging_dir / "spatial" / "masks.index.jsonl") == 0);
  assert(std::filesystem::file_size(staging_dir / "embeddings" / "embeddings.index.jsonl") == 0);
  assert(std::filesystem::file_size(staging_dir / "embeddings" / "embeddings.blocks.svpez") == 0);

  std::ifstream sets_in(staging_dir / "embeddings" / "embedding_sets.json");
  nlohmann::json sets_json;
  sets_in >> sets_json;
  assert(sets_json["sets"].is_array());
  assert(sets_json["sets"].empty());

  const std::vector<nlohmann::json> processors =
      read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  assert(processors.size() >= 2);
  bool found_spatial = false;
  bool found_embedding = false;
  for (const auto& proc : processors) {
    const std::string id = proc.value("id", "");
    if (id == "processor_spatial_placeholder_0001") {
      found_spatial = true;
      assert(proc.value("status", "") == "not_run");
    }
    if (id == "processor_embedding_placeholder_0001") {
      found_embedding = true;
      assert(proc.value("status", "") == "not_run");
    }
  }
  assert(found_spatial);
  assert(found_embedding);

  const nlohmann::json summary_json =
      svp::package::spatial_embedding_placeholder_summary_to_json(summary);
  assert(summary_json["depth_index_written"] == true);
  assert(summary_json["depth_blocks_written"] == false);
  assert(summary_json["depth_placeholder_written"] == true);
  assert(summary_json["masks_blocks_written"] == true);
  assert(summary_json["embedding_sets_written"] == true);
  assert(summary_json["embeddings_blocks_written"] == true);

  std::filesystem::remove_all(root);
}

void test_validation_report_storage() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-validation-report-storage-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);

  nlohmann::json report = {
    {"schema_version", "svp-validation-report-v1"},
    {"status", "invalid"},
    {"core_status", "invalid"},
    {"errors", nlohmann::json::array({
      nlohmann::json{{"code", "ERR_MISSING_DEPTH"},
                    {"severity", "error"},
                    {"path", "/spatial/depth.blocks.svpdz"},
                    {"message", "Required SVPB package entry is absent."}}
    })}
  };

  bool success = svp::package::write_validation_report_to_staging(staging_dir, report);
  assert(success);

  const std::filesystem::path report_path = staging_dir / "provenance" / "validation.json";
  assert(std::filesystem::exists(report_path));

  std::ifstream in(report_path);
  nlohmann::json read_back;
  in >> read_back;
  assert(read_back["schema_version"] == "svp-validation-report-v1");
  assert(read_back["status"] == "invalid");
  assert(read_back["errors"].size() == 1);

  std::filesystem::remove_all(root);
}

void test_package_skeleton_includes_placeholders_and_validation_report() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-package-skeleton-full-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path package_path = root / "output.svp";
  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "provenance");

  [[maybe_unused]] const auto placeholder_summary =
      svp::package::write_spatial_and_embedding_placeholders(staging_dir, false);

  nlohmann::json report = {
    {"schema_version", "svp-validation-report-v1"},
    {"status", "invalid"},
    {"core_status", "invalid"},
    {"errors", nlohmann::json::array()}
  };
  assert(svp::package::write_validation_report_to_staging(staging_dir, report));

  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"},
    {"package_id", "svp_test_full_pkg"},
    {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  bool success = svp::package::write_package_skeleton(
      package_path, staging_dir, "", manifest);
  assert(success);

  auto layout_result = svp::package::read_package_layout(package_path);
  assert(layout_result.has_value());
  const auto& layout = layout_result.value();

  assert(layout.has_entry("spatial/depth.index.jsonl"));
  assert(layout.has_entry("spatial/depth.blocks.svpdz"));
  assert(layout.has_entry("spatial/masks.index.jsonl"));
  assert(layout.has_entry("spatial/masks.blocks.svpmz"));
  assert(layout.has_entry("embeddings/embedding_sets.json"));
  assert(layout.has_entry("embeddings/embeddings.index.jsonl"));
  assert(layout.has_entry("embeddings/embeddings.blocks.svpez"));
  assert(layout.has_entry("provenance/validation.json"));

  std::filesystem::remove_all(root);
}

void test_timeline_writer() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-timeline-writer-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);

  svp::media::MediaIngestPlan plan;
  plan.source_path = "test.mp4";
  plan.primary_video_stream.id = "vstream_0001";
  plan.primary_video_stream.width = 1920;
  plan.primary_video_stream.height = 1080;
  plan.primary_video_stream.rotation_degrees = 90;
  plan.primary_video_stream.pixel_aspect_ratio = {1, 1};
  plan.canonical_raster.width = 360;
  plan.canonical_raster.height = 640;
  plan.canonical_raster.display.oriented_width = 1080;
  plan.canonical_raster.display.oriented_height = 1920;

  svp::vision::FoundationColorStagingArtifact color_artifact =
      svp::vision::build_foundation_color_staging_artifact();

  // Write timeline
  svp::package::TimelineWriteSummary summary =
      svp::package::write_timeline_artifacts(staging_dir, plan, color_artifact);

  assert(summary.frames_written);
  assert(summary.shots_written);
  assert(summary.scenes_written);
  assert(summary.frame_count == 3);
  assert(summary.shot_count == 2);
  assert(summary.scene_count == 1);

  // Check frames.jsonl
  auto frames = read_jsonl_records(staging_dir / "timeline" / "frames.jsonl");
  assert(frames.size() == 3);
  assert(frames[0].value("id", "") == "frame_000001");
  assert(frames[0].value("frame_index", -1) == 0);
  assert(frames[0].value("pts_us", -1) == 0);
  assert(frames[0].value("pts_sec", "") == "0.000000");
  assert(frames[0].value("source_width", -1) == 1920);
  assert(frames[0].value("source_height", -1) == 1080);
  assert(frames[0].value("display_width", -1) == 1080);
  assert(frames[0].value("display_height", -1) == 1920);
  assert(frames[0].value("analysis_width", -1) == 360);
  assert(frames[0].value("analysis_height", -1) == 640);
  assert(frames[0].value("shot_id", "") == "shot_000001");
  assert(frames[0].value("scene_id", "") == "scene_000001");

  // Check shots.jsonl
  auto shots = read_jsonl_records(staging_dir / "timeline" / "shots.jsonl");
  assert(shots.size() == 2);

  // 1. Shots must be non-zero intervals
  assert(shots[0].value("start_us", -1) == 0);
  assert(shots[0].value("end_us", -1) == 500000);
  assert(shots[0].value("start_us", -1) < shots[0].value("end_us", -1));

  assert(shots[1].value("start_us", -1) == 500000);
  assert(shots[1].value("end_us", -1) == 1000000);
  assert(shots[1].value("start_us", -1) < shots[1].value("end_us", -1));

  // 2. Shot intervals are sorted and do not overlap
  assert(shots[0].value("end_us", -1) == shots[1].value("start_us", -1));

  // 3. First shot starts at 0, last shot ends at media duration
  assert(shots[0].value("start_us", -1) == 0);
  assert(shots[1].value("end_us", -1) == 1000000);

  // 4. source_start/source_end labels are only used when interval semantics cover source boundaries
  assert(shots[0].value("cut_type_in", "") == "source_start");
  assert(shots[0].value("cut_type_out", "") == "hard_cut");
  assert(shots[1].value("cut_type_in", "") == "hard_cut");
  assert(shots[1].value("cut_type_out", "") == "source_end");

  // Check scenes.jsonl
  auto scenes = read_jsonl_records(staging_dir / "timeline" / "scenes.jsonl");
  assert(scenes.size() == 1);
  assert(scenes[0].value("id", "") == "scene_000001");

  // 5. Scenes contain their shot intervals coherently
  assert(scenes[0].value("start_us", -1) == shots[0].value("start_us", -1));
  assert(scenes[0].value("end_us", -1) == shots[1].value("end_us", -1));
  assert(scenes[0]["shot_ids"].size() == 2);
  assert(scenes[0]["shot_ids"][0] == "shot_000001");
  assert(scenes[0]["shot_ids"][1] == "shot_000002");

  // Check processors.jsonl
  auto processors = read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  bool found_timeline_proc = false;
  for (const auto& proc : processors) {
    if (proc.value("id", "") == "processor_timeline_generator_0001") {
      found_timeline_proc = true;
      assert(proc.value("name", "") == "svp timeline generator");
    }
  }
  assert(found_timeline_proc);

  std::filesystem::remove_all(root);
}

void test_entity_writer_basic() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-basic-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Write text regions
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.95},
        {"shot_id", "shot_000001"},
        {"scene_id", "scene_000001"},
        {"bbox_norm", {0.1, 0.2, 0.3, 0.4}}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_region_id", "tr_000002"},
        {"start_us", 3000000},
        {"end_us", 4000000},
        {"confidence", 0.88},
        {"shot_id", "shot_000002"},
        {"scene_id", "scene_000001"},
        {"bbox_norm", {0.1, 0.2, 0.3, 0.4}}
    }).dump() << "\n";
  }

  // Write text observations linking regions to normalized text
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json({
        {"text_observation_id", "tobs_000001"},
        {"text_region_id", "tr_000001"},
        {"normalized_text", "HELLO"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_observation_id", "tobs_000002"},
        {"text_region_id", "tr_000002"},
        {"normalized_text", "HELLO"}
    }).dump() << "\n";
  }

  // Write timeline shots for media duration
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json({
        {"id", "shot_000001"},
        {"start_us", 0},
        {"end_us", 5000000}
    }).dump() << "\n";
  }

  const svp::package::EntityWriteSummary summary =
      svp::package::write_entity_artifacts(staging_dir);

  assert(summary.entities_written);
  assert(summary.tracks_written);
  assert(summary.entity_count == 1);
  assert(summary.track_count == 1);

  // Read entities
  auto entities = read_jsonl_records(staging_dir / "entities" / "entities.jsonl");
  assert(entities.size() == 1);
  assert(entities[0].value("id", "") == "entity_000001");
  assert(entities[0].value("entity_type", "") == "unknown_region");
  assert(entities[0].value("first_seen_us", -1) == 1000000);
  assert(entities[0].value("last_seen_us", -1) == 4000000);
  assert(entities[0]["track_ids"].size() == 1);
  assert(entities[0]["track_ids"][0] == "track_000001_a");
  assert(entities[0].contains("evidence"));
  assert(entities[0]["evidence"].value("evidence_type", "") == "ocr_text_region");
  assert(entities[0]["evidence"].value("normalized_text", "") == "HELLO");
  assert(entities[0]["evidence"]["region_ids"].size() == 2);

  // Read tracks
  auto tracks = read_jsonl_records(staging_dir / "entities" / "entity_tracks.jsonl");
  assert(tracks.size() == 1);
  assert(tracks[0].value("id", "") == "track_000001_a");
  assert(tracks[0].value("entity_id", "") == "entity_000001");
  assert(tracks[0].value("start_us", -1) == 1000000);
  assert(tracks[0].value("end_us", -1) == 4000000);
  assert(tracks[0].value("region_count", -1) == 2);
  assert(tracks[0].contains("evidence"));
  assert(tracks[0]["evidence"].value("tracking_method", "") == "text_content_match");

  // Check processor provenance
  auto processors = read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  bool found_entity_proc = false;
  for (const auto& proc : processors) {
    if (proc.value("id", "") == "processor_entity_writer_0001") {
      found_entity_proc = true;
      assert(proc.contains("provenance_note"));
    }
  }
  assert(found_entity_proc);

  std::filesystem::remove_all(root);
}

void test_entity_writer_empty_input() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-empty-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "provenance");

  const svp::package::EntityWriteSummary summary =
      svp::package::write_entity_artifacts(staging_dir);

  assert(summary.entities_written);
  assert(summary.tracks_written);
  assert(summary.entity_count == 0);
  assert(summary.track_count == 0);

  // Files should exist but be empty
  assert(std::filesystem::exists(staging_dir / "entities" / "entities.jsonl"));
  assert(std::filesystem::exists(staging_dir / "entities" / "entity_tracks.jsonl"));
  assert(std::filesystem::file_size(staging_dir / "entities" / "entities.jsonl") == 0);
  assert(std::filesystem::file_size(staging_dir / "entities" / "entity_tracks.jsonl") == 0);

  // Processor should still be written
  auto processors = read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  bool found_entity_proc = false;
  for (const auto& proc : processors) {
    if (proc.value("id", "") == "processor_entity_writer_0001") {
      found_entity_proc = true;
    }
  }
  assert(found_entity_proc);

  std::filesystem::remove_all(root);
}

void test_entity_writer_deterministic_ids() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-determinism-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir_1 = root / "staging1";
  const std::filesystem::path staging_dir_2 = root / "staging2";

  for (const auto& sd : {staging_dir_1, staging_dir_2}) {
    std::filesystem::create_directories(sd / "text");
    std::filesystem::create_directories(sd / "timeline");
    std::filesystem::create_directories(sd / "provenance");

    {
      std::ofstream out(sd / "text" / "text_regions.jsonl");
      out << nlohmann::json({
          {"text_region_id", "tr_000001"},
          {"start_us", 1000000},
          {"end_us", 2000000},
          {"confidence", 0.9}
      }).dump() << "\n";
      out << nlohmann::json({
          {"text_region_id", "tr_000002"},
          {"start_us", 3000000},
          {"end_us", 4000000},
          {"confidence", 0.8}
      }).dump() << "\n";
    }
    {
      std::ofstream out(sd / "text" / "text_observations.jsonl");
      out << nlohmann::json({
          {"text_observation_id", "tobs_000001"},
          {"text_region_id", "tr_000001"},
          {"normalized_text", "WORLD"}
      }).dump() << "\n";
      out << nlohmann::json({
          {"text_observation_id", "tobs_000002"},
          {"text_region_id", "tr_000002"},
          {"normalized_text", "WORLD"}
      }).dump() << "\n";
    }
    {
      std::ofstream out(sd / "timeline" / "shots.jsonl");
      out << nlohmann::json({
          {"id", "shot_000001"},
          {"start_us", 0},
          {"end_us", 5000000}
      }).dump() << "\n";
    }
  }

  const auto summary1 = svp::package::write_entity_artifacts(staging_dir_1);
  const auto summary2 = svp::package::write_entity_artifacts(staging_dir_2);

  assert(summary1.entity_count == summary2.entity_count);
  assert(summary1.track_count == summary2.track_count);

  auto entities1 = read_jsonl_records(staging_dir_1 / "entities" / "entities.jsonl");
  auto entities2 = read_jsonl_records(staging_dir_2 / "entities" / "entities.jsonl");
  assert(entities1.size() == entities2.size());
  for (std::size_t i = 0; i < entities1.size(); ++i) {
    assert(entities1[i].value("id", "") == entities2[i].value("id", ""));
    assert(entities1[i].value("first_seen_us", -1) == entities2[i].value("first_seen_us", -1));
    assert(entities1[i].value("last_seen_us", -1) == entities2[i].value("last_seen_us", -1));
  }

  auto tracks1 = read_jsonl_records(staging_dir_1 / "entities" / "entity_tracks.jsonl");
  auto tracks2 = read_jsonl_records(staging_dir_2 / "entities" / "entity_tracks.jsonl");
  assert(tracks1.size() == tracks2.size());
  for (std::size_t i = 0; i < tracks1.size(); ++i) {
    assert(tracks1[i].value("id", "") == tracks2[i].value("id", ""));
    assert(tracks1[i].value("entity_id", "") == tracks2[i].value("entity_id", ""));
  }

  std::filesystem::remove_all(root);
}

void test_entity_writer_no_dangling_refs() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-dangling-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Write text regions with frame references
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.9},
        {"frame_start", "frame_000001"},
        {"frame_end", "frame_000002"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json({
        {"text_observation_id", "tobs_000001"},
        {"text_region_id", "tr_000001"},
        {"normalized_text", "TEST"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json({
        {"id", "shot_000001"},
        {"start_us", 0},
        {"end_us", 5000000}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "frames.jsonl");
    out << nlohmann::json({
        {"id", "frame_000001"},
        {"frame_index", 0},
        {"pts_us", 1000000}
    }).dump() << "\n";
    out << nlohmann::json({
        {"id", "frame_000002"},
        {"frame_index", 1},
        {"pts_us", 2000000}
    }).dump() << "\n";
  }

  const svp::package::EntityWriteSummary summary =
      svp::package::write_entity_artifacts(staging_dir);

  assert(summary.entity_count == 1);
  assert(summary.track_count == 1);

  auto tracks = read_jsonl_records(staging_dir / "entities" / "entity_tracks.jsonl");
  assert(tracks.size() == 1);
  // Track should have frame references from the text region
  assert(tracks[0].value("start_frame_id", "") == "frame_000001");
  assert(tracks[0].value("end_frame_id", "") == "frame_000002");

  // Verify entity track references resolve: track.entity_id must match entity.id
  auto entities = read_jsonl_records(staging_dir / "entities" / "entities.jsonl");
  assert(entities.size() == 1);
  assert(tracks[0].value("entity_id", "") == entities[0].value("id", ""));

  // Verify entity.track_ids must contain the track id
  bool track_found = false;
  for (const auto& tid : entities[0]["track_ids"]) {
    if (tid == tracks[0].value("id", "")) {
      track_found = true;
    }
  }
  assert(track_found);

  std::filesystem::remove_all(root);
}

void test_entity_writer_multiple_groups() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-multi-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.9}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_region_id", "tr_000002"},
        {"start_us", 3000000},
        {"end_us", 4000000},
        {"confidence", 0.8}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_region_id", "tr_000003"},
        {"start_us", 5000000},
        {"end_us", 6000000},
        {"confidence", 0.7}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json({
        {"text_observation_id", "tobs_000001"},
        {"text_region_id", "tr_000001"},
        {"normalized_text", "AAA"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_observation_id", "tobs_000002"},
        {"text_region_id", "tr_000002"},
        {"normalized_text", "BBB"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_observation_id", "tobs_000003"},
        {"text_region_id", "tr_000003"},
        {"normalized_text", "AAA"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json({
        {"id", "shot_000001"},
        {"start_us", 0},
        {"end_us", 10000000}
    }).dump() << "\n";
  }

  const svp::package::EntityWriteSummary summary =
      svp::package::write_entity_artifacts(staging_dir);

  // Two groups: "AAA" (tr_000001 + tr_000003) and "BBB" (tr_000002)
  assert(summary.entity_count == 2);
  assert(summary.track_count == 2);

  auto entities = read_jsonl_records(staging_dir / "entities" / "entities.jsonl");
  assert(entities.size() == 2);

  // First entity should be the one with earliest start_us
  assert(entities[0].value("id", "") == "entity_000001");
  assert(entities[0]["evidence"].value("normalized_text", "") == "AAA");
  assert(entities[0]["evidence"]["region_ids"].size() == 2);

  assert(entities[1].value("id", "") == "entity_000002");
  assert(entities[1]["evidence"].value("normalized_text", "") == "BBB");
  assert(entities[1]["evidence"]["region_ids"].size() == 1);

  std::filesystem::remove_all(root);
}

void test_entity_writer_missing_observations() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-missing-obs-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Write text regions but NO text observations
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.9}
    }).dump() << "\n";
  }

  const svp::package::EntityWriteSummary summary =
      svp::package::write_entity_artifacts(staging_dir);

  // No observations means no evidence to group by — should produce empty entities
  assert(summary.entity_count == 0);
  assert(summary.track_count == 0);
  assert(summary.skipped_missing_evidence > 0);

  // Files should still exist
  assert(std::filesystem::exists(staging_dir / "entities" / "entities.jsonl"));
  assert(std::filesystem::exists(staging_dir / "entities" / "entity_tracks.jsonl"));

  std::filesystem::remove_all(root);
}

void test_entity_writer_provenance_honest() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-provenance-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.9}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json({
        {"text_observation_id", "tobs_000001"},
        {"text_region_id", "tr_000001"},
        {"normalized_text", "X"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json({
        {"id", "shot_000001"},
        {"start_us", 0},
        {"end_us", 5000000}
    }).dump() << "\n";
  }

  (void)svp::package::write_entity_artifacts(staging_dir);

  auto processors = read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  bool found_entity_proc = false;
  for (const auto& proc : processors) {
    if (proc.value("id", "") == "processor_entity_writer_0001") {
      found_entity_proc = true;
      assert(proc.contains("provenance_note"));
      const std::string note = proc.value("provenance_note", "");
      // Provenance must honestly state that no object recognition is used
      assert(note.find("no object recognition") != std::string::npos ||
             note.find("not") != std::string::npos);
      assert(proc.contains("input_refs"));
      assert(proc.contains("output_refs"));
    }
  }
  assert(found_entity_proc);

  std::filesystem::remove_all(root);
}

void test_entity_writer_string_frame_id_validated() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-str-frame-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Write timeline frames with known IDs
  {
    std::ofstream out(staging_dir / "timeline" / "frames.jsonl");
    out << nlohmann::json({
        {"id", "frame_000001"},
        {"frame_index", 0},
        {"pts_us", 1000000}
    }).dump() << "\n";
    out << nlohmann::json({
        {"id", "frame_000002"},
        {"frame_index", 1},
        {"pts_us", 2000000}
    }).dump() << "\n";
  }

  // Write text regions: one with a valid string frame ID, one with a fabricated one
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.9},
        {"frame_start", "frame_000001"},
        {"frame_end", "frame_000002"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_region_id", "tr_000002"},
        {"start_us", 3000000},
        {"end_us", 4000000},
        {"confidence", 0.8},
        {"frame_start", "frame_FAKE_NOT_IN_TIMELINE"},
        {"frame_end", "frame_ALSO_FAKE"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json({
        {"text_observation_id", "tobs_000001"},
        {"text_region_id", "tr_000001"},
        {"normalized_text", "VALID"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_observation_id", "tobs_000002"},
        {"text_region_id", "tr_000002"},
        {"normalized_text", "FAKE_FRAME"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json({
        {"id", "shot_000001"},
        {"start_us", 0},
        {"end_us", 5000000}
    }).dump() << "\n";
  }

  const svp::package::EntityWriteSummary summary =
      svp::package::write_entity_artifacts(staging_dir);

  assert(summary.entity_count == 2);
  assert(summary.track_count == 2);

  auto tracks = read_jsonl_records(staging_dir / "entities" / "entity_tracks.jsonl");
  assert(tracks.size() == 2);

  // First track (VALID) should have frame IDs
  assert(tracks[0].value("start_frame_id", "") == "frame_000001");
  assert(tracks[0].value("end_frame_id", "") == "frame_000002");

  // Second track (FAKE_FRAME) should NOT have frame IDs — they were unresolved
  assert(!tracks[1].contains("start_frame_id"));
  assert(!tracks[1].contains("end_frame_id"));

  std::filesystem::remove_all(root);
}

void test_entity_writer_deterministic_tiebreakers() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-entity-writer-tiebreaker-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir / "text");
  std::filesystem::create_directories(staging_dir / "timeline");
  std::filesystem::create_directories(staging_dir / "provenance");

  // Write timeline frames
  {
    std::ofstream out(staging_dir / "timeline" / "frames.jsonl");
    out << nlohmann::json({
        {"id", "frame_000001"},
        {"frame_index", 0},
        {"pts_us", 1000000}
    }).dump() << "\n";
  }

  // Three text regions all with the SAME start_us but different text/IDs
  {
    std::ofstream out(staging_dir / "text" / "text_regions.jsonl");
    out << nlohmann::json({
        {"text_region_id", "tr_000003"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.7}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_region_id", "tr_000001"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.9}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_region_id", "tr_000002"},
        {"start_us", 1000000},
        {"end_us", 2000000},
        {"confidence", 0.8}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "text" / "text_observations.jsonl");
    out << nlohmann::json({
        {"text_observation_id", "tobs_000001"},
        {"text_region_id", "tr_000001"},
        {"normalized_text", "CCC"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_observation_id", "tobs_000002"},
        {"text_region_id", "tr_000002"},
        {"normalized_text", "AAA"}
    }).dump() << "\n";
    out << nlohmann::json({
        {"text_observation_id", "tobs_000003"},
        {"text_region_id", "tr_000003"},
        {"normalized_text", "BBB"}
    }).dump() << "\n";
  }
  {
    std::ofstream out(staging_dir / "timeline" / "shots.jsonl");
    out << nlohmann::json({
        {"id", "shot_000001"},
        {"start_us", 0},
        {"end_us", 5000000}
    }).dump() << "\n";
  }

  // Build twice to verify determinism
  const std::filesystem::path staging_dir_2 = root / "staging2";
  std::filesystem::create_directories(staging_dir_2 / "text");
  std::filesystem::create_directories(staging_dir_2 / "timeline");
  std::filesystem::create_directories(staging_dir_2 / "provenance");
  std::filesystem::copy(
      staging_dir / "text" / "text_regions.jsonl",
      staging_dir_2 / "text" / "text_regions.jsonl",
      std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy(
      staging_dir / "text" / "text_observations.jsonl",
      staging_dir_2 / "text" / "text_observations.jsonl",
      std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy(
      staging_dir / "timeline" / "frames.jsonl",
      staging_dir_2 / "timeline" / "frames.jsonl",
      std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy(
      staging_dir / "timeline" / "shots.jsonl",
      staging_dir_2 / "timeline" / "shots.jsonl",
      std::filesystem::copy_options::overwrite_existing);

  const auto summary1 = svp::package::write_entity_artifacts(staging_dir);
  const auto summary2 = svp::package::write_entity_artifacts(staging_dir_2);

  assert(summary1.entity_count == 3);
  assert(summary2.entity_count == 3);

  auto entities1 = read_jsonl_records(staging_dir / "entities" / "entities.jsonl");
  auto entities2 = read_jsonl_records(staging_dir_2 / "entities" / "entities.jsonl");
  assert(entities1.size() == 3);
  assert(entities2.size() == 3);

  // All three groups have the same start_us (1000000).
  // Tie-breaker is normalized_text, so order should be AAA, BBB, CCC.
  assert(entities1[0].value("id", "") == "entity_000001");
  assert(entities1[0]["evidence"].value("normalized_text", "") == "AAA");
  assert(entities1[1].value("id", "") == "entity_000002");
  assert(entities1[1]["evidence"].value("normalized_text", "") == "BBB");
  assert(entities1[2].value("id", "") == "entity_000003");
  assert(entities1[2]["evidence"].value("normalized_text", "") == "CCC");

  // Verify both builds produce identical IDs
  for (std::size_t i = 0; i < entities1.size(); ++i) {
    assert(entities1[i].value("id", "") == entities2[i].value("id", ""));
  }

  auto tracks1 = read_jsonl_records(staging_dir / "entities" / "entity_tracks.jsonl");
  auto tracks2 = read_jsonl_records(staging_dir_2 / "entities" / "entity_tracks.jsonl");
  for (std::size_t i = 0; i < tracks1.size(); ++i) {
    assert(tracks1[i].value("id", "") == tracks2[i].value("id", ""));
    assert(tracks1[i].value("entity_id", "") == tracks2[i].value("entity_id", ""));
  }

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_write_package_skeleton_creates_atomic_zip_file();
  test_writer_fails_gracefully_on_missing_staging_dir();
  test_writer_fails_on_rename_and_cleans_up_temp();
  test_write_relationships_and_provenance();
  test_full_relationship_graph();
  test_depth_index_alone_no_frame_ids();
  test_color_frame_target_skipped_without_timeline();
  test_text_region_shot_scene_skipped_without_timeline();
  test_timeline_present_enables_relationships();
  test_unmatched_word_segment_counting();
  test_relationships_with_missing_sections();
  test_relationships_dangling_reference_prevention();
  test_write_index_foundation();
  test_spatial_embedding_placeholders();
  test_timeline_writer();
  test_validation_report_storage();
  test_package_skeleton_includes_placeholders_and_validation_report();
  test_entity_writer_basic();
  test_entity_writer_empty_input();
  test_entity_writer_deterministic_ids();
  test_entity_writer_no_dangling_refs();
  test_entity_writer_multiple_groups();
  test_entity_writer_missing_observations();
  test_entity_writer_provenance_honest();
  test_entity_writer_string_frame_id_validated();
  test_entity_writer_deterministic_tiebreakers();
  std::cout << "All svp-package-tests passed!\n";
  return 0;
}
