#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/report.hpp"
#include "svp/validation/validator.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

std::filesystem::path create_test_package(const std::string& test_name,
                                          const nlohmann::json& transcript_json) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / test_name;
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path package_path = root / "test.svp";
  const std::filesystem::path staging_dir = root / "staging";

  write_file(staging_dir / "transcript" / "transcript.json",
             transcript_json.dump());

  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"},
    {"package_id", "svp_test_diarization"},
    {"created_utc", "2026-06-22T00:00:00Z"},
    {"primary_media_id", "media_000001"},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }}
  };

  bool success = svp::package::write_package_skeleton(
      package_path, staging_dir, "", manifest);
  assert(success);
  assert(std::filesystem::exists(package_path));

  return package_path;
}

bool has_finding_with_code(const svp::validation::ValidationReport& report,
                           const std::string& code) {
  for (const auto& f : report.errors) {
    if (f.code == code) return true;
  }
  for (const auto& f : report.warnings) {
    if (f.code == code) return true;
  }
  for (const auto& f : report.infos) {
    if (f.code == code) return true;
  }
  return false;
}

void test_validator_emits_warning_for_fallback_diarization() {
  nlohmann::json transcript = {
    {"diarization", {{"status", "fallback_one_speaker"}, {"one_speaker_fallback", true}}},
    {"speaker_count", 1}
  };

  const auto package_path =
      create_test_package("svp-diar-validation-fallback", transcript);

  const std::filesystem::path repo_root =
      std::filesystem::current_path().parent_path().parent_path();

  svp::validation::ValidatorOptions options;
  options.validation_codes_path =
      repo_root / "spec" / "registries" / "validation-codes.json";
  options.registry_root_path = repo_root / "spec" / "registries";
  options.schema_root_path = repo_root / "spec" / "schemas";

  svp::validation::ValidationReport report =
      svp::validation::validate_package(package_path, options);

  assert(has_finding_with_code(report, "WARN_DIARIZATION_FALLBACK"));

  std::filesystem::remove_all(package_path.parent_path());
}

void test_validator_emits_error_for_unavailable_diarization() {
  nlohmann::json transcript = {
    {"diarization", {{"status", "unavailable"}}},
    {"speaker_count", 0}
  };

  const auto package_path =
      create_test_package("svp-diar-validation-unavailable", transcript);

  const std::filesystem::path repo_root =
      std::filesystem::current_path().parent_path().parent_path();

  svp::validation::ValidatorOptions options;
  options.validation_codes_path =
      repo_root / "spec" / "registries" / "validation-codes.json";
  options.registry_root_path = repo_root / "spec" / "registries";
  options.schema_root_path = repo_root / "spec" / "schemas";

  svp::validation::ValidationReport report =
      svp::validation::validate_package(package_path, options);

  assert(has_finding_with_code(report, "ERR_DIARIZATION_UNAVAILABLE"));

  std::filesystem::remove_all(package_path.parent_path());
}

void test_validator_emits_error_for_unparsable_transcript() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-diar-validation-unparsable";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const std::filesystem::path package_path = root / "test.svp";
  const std::filesystem::path staging_dir = root / "staging";

  write_file(staging_dir / "transcript" / "transcript.json", "{not valid json}");

  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"},
    {"package_id", "svp_test_diarization_unparsable"},
    {"created_utc", "2026-06-22T00:00:00Z"},
    {"primary_media_id", "media_000001"},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }}
  };

  bool success = svp::package::write_package_skeleton(
      package_path, staging_dir, "", manifest);
  assert(success);

  const std::filesystem::path repo_root =
      std::filesystem::current_path().parent_path().parent_path();

  svp::validation::ValidatorOptions options;
  options.validation_codes_path =
      repo_root / "spec" / "registries" / "validation-codes.json";
  options.registry_root_path = repo_root / "spec" / "registries";
  options.schema_root_path = repo_root / "spec" / "schemas";

  svp::validation::ValidationReport report =
      svp::validation::validate_package(package_path, options);

  assert(has_finding_with_code(report, "ERR_DIARIZATION_UNAVAILABLE"));

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_validator_emits_warning_for_fallback_diarization();
  test_validator_emits_error_for_unavailable_diarization();
  test_validator_emits_error_for_unparsable_transcript();
  return 0;
}
