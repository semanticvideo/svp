#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"
#include <nlohmann/json.hpp>

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
  assert(summary.relationships_written == 2);
  assert(summary.processors_written == 3);
  assert(summary.duplicate_processors_merged == 1);

  const std::vector<nlohmann::json> relationships =
      read_jsonl_records(staging_dir / "relationships" / "relationships.jsonl");
  assert(relationships.size() == 2);
  assert(relationships[0]["source_id"] == "text_region_001");
  assert(relationships[0]["processor_id"] == "processor_relationship_writer_0001");
  assert(relationships[0]["type"] == "appears_in_scene" ||
         relationships[0]["type"] == "appears_in_shot");

  const std::vector<nlohmann::json> processors =
      read_jsonl_records(staging_dir / "provenance" / "processors.jsonl");
  assert(processors.size() == 3);
  assert(processors[0]["id"] == "processor_color_0001");
  assert(processors[1]["id"] == "processor_ocr_0001");
  assert(processors[1]["version"] == "a");
  assert(processors[2]["id"] == "processor_relationship_writer_0001");

  const std::filesystem::path package_path = root / "output.svp";
  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"},
    {"package_id", "svp_relationship_test_pkg"}
  };
  assert(svp::package::write_package_skeleton(package_path, staging_dir, "", manifest));
  auto layout_result = svp::package::read_package_layout(package_path);
  assert(layout_result.has_value());
  assert(layout_result.value().has_entry("relationships/relationships.jsonl"));
  assert(layout_result.value().has_entry("provenance/processors.jsonl"));

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

  nlohmann::json manifest = {
    {"package_id", "svp_test_pkg_id"},
    {"created_utc", "2026-06-20T00:00:00Z"}
  };

  const svp::package::RelationshipProvenanceWriteSummary relationship_summary =
      svp::package::write_relationships_and_provenance(staging_dir);
  assert(relationship_summary.relationships_written == 2);

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
  assert(index_manifest["table_count"] == 13);
  assert(index_manifest["row_count"] > 0);
  assert(index_manifest["created_from"]["manifest_blake3"].get<std::string>().find("blake3:") == 0);

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_write_package_skeleton_creates_atomic_zip_file();
  test_writer_fails_gracefully_on_missing_staging_dir();
  test_writer_fails_on_rename_and_cleans_up_temp();
  test_write_relationships_and_provenance();
  test_write_index_foundation();
  std::cout << "All svp-package-tests passed!\n";
  return 0;
}
