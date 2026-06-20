#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

}  // namespace

int main() {
  test_write_package_skeleton_creates_atomic_zip_file();
  test_writer_fails_gracefully_on_missing_staging_dir();
  std::cout << "All svp-package-tests passed!\n";
  return 0;
}
