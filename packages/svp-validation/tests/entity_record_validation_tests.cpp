#include "svp/package/package_writer.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/validator.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << content;
}

bool has_error(const svp::validation::ValidationReport& report,
               std::string_view code) {
  return std::any_of(report.errors.begin(), report.errors.end(),
                     [&](const auto& finding) { return finding.code == code; });
}

void test_rejects_non_conforming_entity_type() {
  const auto root =
      std::filesystem::temp_directory_path() / "svp-invalid-entity-type-test";
  std::filesystem::remove_all(root);
  const auto staging = root / "staging";
  const auto package = root / "test.svp";
  write_file(staging / "entities" / "entities.jsonl",
             R"({"id":"entity_1","entity_type":"dynamic_group"})"
             "\n");
  const nlohmann::json manifest = {{"svp_version", "1.0-rc.2"},
                                   {"package_id", "svp_test_entity_type"},
                                   {"created_utc", "2026-06-22T00:00:00Z"},
                                   {"primary_media_id", "media_000001"},
                                   {"timebase",
                                    {{"unit", "microseconds"},
                                     {"origin", "primary_presentation_start"},
                                     {"source_timebase_mode", "exact_rational"},
                                     {"rounding", "round_half_to_even"}}}};
  assert(svp::package::write_package_skeleton(package, staging, "", manifest));

  const auto repo_root =
      std::filesystem::current_path().parent_path().parent_path();
  svp::validation::ValidatorOptions options;
  options.validation_codes_path =
      repo_root / "spec" / "registries" / "validation-codes.json";
  options.registry_root_path = repo_root / "spec" / "registries";
  options.schema_root_path = repo_root / "spec" / "schemas";
  const auto report = svp::validation::validate_package(package, options);
  assert(has_error(report, svp::validation::kCodeInvalidEntityRecord));
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_rejects_non_conforming_entity_type();
  return 0;
}
