// RC2 §23.5: provenance/capture.json is optional, and validates against
// capture.schema.json when present. Every value here is made up.

#include "svp/package/package_writer.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/validator.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << content;
}

bool has_capture_error(const svp::validation::ValidationReport& report) {
  return std::any_of(report.errors.begin(), report.errors.end(), [](const auto& finding) {
    return finding.code == svp::validation::kCodeProvenanceInvalidCapture;
  });
}

// A skeleton package, with `capture` as provenance/capture.json when given.
svp::validation::ValidationReport validate_with(const std::string& name,
                                                const std::optional<std::string>& capture) {
  const auto root = std::filesystem::temp_directory_path() / ("svp-capture-" + name);
  std::filesystem::remove_all(root);
  const auto staging = root / "staging";
  std::filesystem::create_directories(staging);
  if (capture) {
    write_file(staging / "provenance" / "capture.json", *capture);
  }
  const nlohmann::json manifest = {{"svp_version", "1.0-rc.2"},
                                   {"package_id", "svp_test_capture"},
                                   {"created_utc", "2026-10-26T00:00:00Z"},
                                   {"primary_media_id", "media_000001"},
                                   {"timebase",
                                    {{"unit", "microseconds"},
                                     {"origin", "primary_presentation_start"},
                                     {"source_timebase_mode", "exact_rational"},
                                     {"rounding", "round_half_to_even"}}}};
  const auto package = root / "test.svp";
  assert(svp::package::write_package_skeleton(package, staging, "", manifest));

  const auto repo_root = std::filesystem::current_path().parent_path().parent_path();
  svp::validation::ValidatorOptions options;
  options.validation_codes_path = repo_root / "spec" / "registries" / "validation-codes.json";
  options.registry_root_path = repo_root / "spec" / "registries";
  options.schema_root_path = repo_root / "spec" / "schemas";
  auto report = svp::validation::validate_package(package, options);
  std::filesystem::remove_all(root);
  return report;
}

const char* kValid = R"({
  "schema_version": "svp-capture-v1",
  "camera": {"make": "Maker", "model": "Cam 1", "software": "1.0"},
  "lens": {"model": "Lens 24mm"},
  "recorded": "2026-01-02T03:04:05-08:00",
  "start_timecode": "01:02:03:04",
  "location": {"latitude": 12.5, "longitude": -45.25, "altitude_m": -3.5,
               "horizontal_accuracy_m": 4.5, "fix": "3d",
               "fixed_utc": "2026-01-02T11:04:03Z"},
  "sources": ["sony_nonrealtime_meta", "container_tags"]
})";

void test_absent_capture_is_fine() {
  assert(!has_capture_error(validate_with("absent", std::nullopt)));
}

void test_valid_capture_passes() {
  assert(!has_capture_error(validate_with("valid", std::string(kValid))));
}

void test_invalid_capture_is_an_error() {
  nlohmann::json bad = nlohmann::json::parse(kValid);
  bad["location"]["latitude"] = 95.0;
  assert(has_capture_error(validate_with("latitude", bad.dump())));

  bad = nlohmann::json::parse(kValid);
  bad["recorded"] = "2026-01-02 03:04:05";
  assert(has_capture_error(validate_with("recorded", bad.dump())));

  bad = nlohmann::json::parse(kValid);
  bad["camera"]["serial"] = "123";
  assert(has_capture_error(validate_with("unknown-field", bad.dump())));

  assert(has_capture_error(validate_with("unreadable", std::string("{not json"))));
}

}  // namespace

int main() {
  test_absent_capture_is_fine();
  test_valid_capture_passes();
  test_invalid_capture_is_an_error();
  return 0;
}
