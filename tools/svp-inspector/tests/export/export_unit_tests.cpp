// Unit tests of the export's policies: entry-name safety, output collisions,
// record splicing, version support, and representation choice.

#include "export_test_support.hpp"

#include "export/entry_path_policy.hpp"
#include "export/export_error.hpp"
#include "export/export_plan.hpp"
#include "export/jsonl_line_reader.hpp"
#include "export/jsonl_record.hpp"
#include "export/previous_export.hpp"
#include "export/reference_rules.hpp"
#include "export/version_policy.hpp"

#include <climits>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using package_export::ExportError;
using package_export::ExportErrorCode;
using nlohmann::json;

ExportErrorCode error_code_of(const std::function<void()>& action) {
  try {
    action();
  } catch (const ExportError& error) {
    return error.code();
  }
  throw std::runtime_error("expected an ExportError");
}

void test_entry_names() {
  for (const auto* name : {"manifest.json", "text/evidence_crops/crop_1.jpg",
                           "extensions/vendor.example/a b.bin"}) {
    package_export::require_safe_entry_name(name, false);
  }
  package_export::require_safe_entry_name("text/evidence_crops/", true);

  const std::vector<std::string> unsafe{
      "",          "/abs.json",  "../escape", "a/../../b", "a//b",
      "./a.json",  "a\\b.json",  std::string{"a\x01" "b"}, "a/.",
      "dir//",
  };
  for (const auto& name : unsafe) {
    EXPORT_CHECK(error_code_of([&] {
                   package_export::require_safe_entry_name(name, false);
                 }) == ExportErrorCode::unsafe_entry_name);
  }
  EXPORT_CHECK(error_code_of([] {
                 package_export::require_safe_entry_name("a/../", true);
               }) == ExportErrorCode::unsafe_entry_name);

  const std::string longest(NAME_MAX, 'n');
  package_export::require_output_segments_fit("layers/" + longest, "entry");
  EXPORT_CHECK(error_code_of([&] {
                 package_export::require_output_segments_fit(
                     "layers/" + longest + "x", "entry");
               }) == ExportErrorCode::unsafe_entry_name);
}

void test_output_collisions() {
  package_export::require_no_output_collisions(
      {{"layers/a.json", "a.json"}, {"layers/b/a.json", "b/a.json"}});
  EXPORT_CHECK(error_code_of([] {
                 package_export::require_no_output_collisions(
                     {{"layers/Text.json", "Text.json"},
                      {"layers/text.json", "text.json"}});
               }) == ExportErrorCode::output_path_collision);
  EXPORT_CHECK(error_code_of([] {
                 package_export::require_no_output_collisions(
                     {{"layers/a", "a"}, {"layers/A/b.json", "A/b.json"}});
               }) == ExportErrorCode::output_path_collision);

  // A block stream's derived files collide with a same-named real entry.
  std::vector<package_export::PackageEntry> entries{
      {.name = "spatial/depth.blocks.svpdz", .zip_index = 0, .size_bytes = 0},
      {.name = "spatial/depth.blocks.svpdz.decoded.bin",
       .zip_index = 1,
       .size_bytes = 0},
  };
  EXPORT_CHECK(error_code_of([&] {
                 (void)package_export::make_export_plan(entries);
               }) == ExportErrorCode::output_path_collision);
}

void test_record_splice() {
  const json resolutions{{"frame_id", {{"found", false}, {"id", "f"}}}};
  const std::string text = R"({"b":1e2,"a":"café"})";
  const auto record = package_export::parse_record(text, "x.jsonl", 1);
  EXPORT_CHECK(package_export::append_export_member(text, record, resolutions) ==
               R"({"b":1e2,"a":"café","svp_export":{"frame_id":{"found":false,"id":"f"}}})");
  const std::string empty = "{ }";
  EXPORT_CHECK(package_export::append_export_member(
                   empty, package_export::parse_record(empty, "x.jsonl", 1),
                   resolutions) ==
               R"({ "svp_export":{"frame_id":{"found":false,"id":"f"}}})");

  EXPORT_CHECK(error_code_of([] {
                 (void)package_export::parse_record("[1]", "x.jsonl", 3);
               }) == ExportErrorCode::malformed_record);
  EXPORT_CHECK(error_code_of([] {
                 (void)package_export::parse_record("{\"a\":", "x.jsonl", 3);
               }) == ExportErrorCode::malformed_record);
  EXPORT_CHECK(error_code_of([] {
                 (void)package_export::parse_record(R"({"svp_export":1})",
                                                    "x.jsonl", 3);
               }) == ExportErrorCode::reserved_member_present);
  EXPORT_CHECK(package_export::trim_json_whitespace(" \t{\"a\":1}\r\n") ==
               "{\"a\":1}");
}

void test_version_policy() {
  using package_export::PackageKind;
  const auto unsupported = [](const json& manifest, PackageKind kind,
                              std::optional<std::uint16_t> profile = {}) {
    return package_export::find_unsupported_declared_version(
        package_export::declared_versions(manifest, kind, profile), kind);
  };
  const auto missing = [](const json& manifest, PackageKind kind,
                          std::optional<std::uint16_t> profile = {}) {
    return package_export::find_missing_required_version(
        package_export::declared_versions(manifest, kind, profile), kind);
  };

  const json svp{{"svp_version", "1.0-rc.2"}};
  EXPORT_CHECK(!unsupported(svp, PackageKind::svp));
  EXPORT_CHECK(!missing(svp, PackageKind::svp));
  EXPORT_CHECK(unsupported({{"svp_version", "1.0-rc.1"}}, PackageKind::svp)
                   ->field == "svp_version");
  EXPORT_CHECK(missing(json::object(), PackageKind::svp)->field == "svp_version");

  const json svpi{{"svpi_version", "0.1"}, {"svp_version", "1.0-rc.2"}};
  EXPORT_CHECK(!unsupported(svpi, PackageKind::svpi));
  EXPORT_CHECK(!missing(svpi, PackageKind::svpi));
  EXPORT_CHECK(!missing({{"svpi_version", "0.1"}}, PackageKind::svpi));
  EXPORT_CHECK(unsupported({{"svpi_version", 0.1}}, PackageKind::svpi)->field ==
               "svpi_version");
  EXPORT_CHECK(missing(json::object(), PackageKind::svpi)->field ==
               "svpi_version");

  EXPORT_CHECK(!unsupported(svpi, PackageKind::embedded_svpi, 1));
  EXPORT_CHECK(unsupported(svpi, PackageKind::embedded_svpi, 2)->field ==
               "embedded_transport_profile_version");
  EXPORT_CHECK(missing(svpi, PackageKind::embedded_svpi)->field ==
               "embedded_transport_profile_version");
}

void test_representations_and_rules() {
  using package_export::Representation;
  using package_export::representation_for;
  EXPORT_CHECK(representation_for("transcript/words.jsonl") ==
               Representation::jsonl);
  EXPORT_CHECK(representation_for("manifest.json") == Representation::json);
  EXPORT_CHECK(representation_for("spatial/depth.blocks.svpdz") ==
               Representation::block_stream);
  EXPORT_CHECK(representation_for("spatial/masks.blocks.svpmz") ==
               Representation::block_stream);
  EXPORT_CHECK(representation_for("embeddings/embeddings.blocks.svpez") ==
               Representation::block_stream);
  EXPORT_CHECK(representation_for("mimetype") == Representation::file);
  EXPORT_CHECK(representation_for("index/index.sqlite") == Representation::file);

  std::set<std::pair<std::string_view, std::string_view>> seen;
  for (const auto& rule : package_export::reference_rules()) {
    EXPORT_CHECK(rule.layer_entry.ends_with(".jsonl"));
    EXPORT_CHECK(seen.emplace(rule.layer_entry, rule.member).second);
  }
  EXPORT_CHECK(package_export::rules_for_layer("transcript/words.jsonl").empty());
  EXPORT_CHECK(package_export::rules_for_layer("text/text_observations.jsonl")
                   .size() == 2);
}

void test_previous_export_guard() {
  namespace fs = std::filesystem;
  export_test::TempDir temp{"previous-export"};
  const std::string summary =
      R"({"export_format":"svp-package-export","export_format_version":1})";
  const auto make = [&](const std::string& name, const std::string& text) {
    const auto directory = temp.path() / name;
    export_test::write_file(directory / "export.json", text);
    fs::create_directories(directory / "layers");
    return directory;
  };

  const auto valid = make("valid", summary);
  EXPORT_CHECK(package_export::is_previous_export(valid));
  // The size bound is inclusive and checked before anything is read.
  EXPORT_CHECK(package_export::is_previous_export(valid, summary.size()));
  EXPORT_CHECK(!package_export::is_previous_export(valid, summary.size() - 1));

  EXPORT_CHECK(!package_export::is_previous_export(temp.path() / "missing"));
  EXPORT_CHECK(!package_export::is_previous_export(make("empty", "")));
  EXPORT_CHECK(!package_export::is_previous_export(
      make("float-version",
           R"({"export_format":"svp-package-export","export_format_version":1.5})")));
  EXPORT_CHECK(!package_export::is_previous_export(
      make("no-format", R"({"export_format_version":1})")));

  // Links never count: not for export.json, and not for layers/.
  const auto linked_summary = temp.path() / "linked-summary";
  fs::create_directories(linked_summary / "layers");
  fs::create_symlink(valid / "export.json", linked_summary / "export.json");
  EXPORT_CHECK(!package_export::is_previous_export(linked_summary));
  const auto linked_layers = temp.path() / "linked-layers";
  export_test::write_file(linked_layers / "export.json", summary);
  fs::create_directory_symlink(valid / "layers", linked_layers / "layers");
  EXPORT_CHECK(!package_export::is_previous_export(linked_layers));

  // layers must be a directory; export.json must be a file.
  const auto file_layers = temp.path() / "file-layers";
  export_test::write_file(file_layers / "export.json", summary);
  export_test::write_file(file_layers / "layers", "not a directory");
  EXPORT_CHECK(!package_export::is_previous_export(file_layers));
  const auto directory_summary = temp.path() / "directory-summary";
  fs::create_directories(directory_summary / "export.json");
  fs::create_directories(directory_summary / "layers");
  EXPORT_CHECK(!package_export::is_previous_export(directory_summary));
}

void test_exit_codes() {
  using package_export::exit_code_for;
  EXPORT_CHECK(exit_code_for(ExportErrorCode::input_unrecognized) == 2);
  EXPORT_CHECK(exit_code_for(ExportErrorCode::package_invalid) == 3);
  EXPORT_CHECK(exit_code_for(ExportErrorCode::unsupported_version) == 4);
  EXPORT_CHECK(exit_code_for(ExportErrorCode::output_exists) == 5);
  EXPORT_CHECK(exit_code_for(ExportErrorCode::unsafe_entry_name) == 6);
  EXPORT_CHECK(exit_code_for(ExportErrorCode::internal_error) == 1);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"entry-names", test_entry_names},
      {"output-collisions", test_output_collisions},
      {"record-splice", test_record_splice},
      {"version-policy", test_version_policy},
      {"representations-and-rules", test_representations_and_rules},
      {"previous-export-guard", test_previous_export_guard},
      {"exit-codes", test_exit_codes},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << name << ": passed\n";
    } catch (const std::exception& error) {
      std::cerr << name << ": " << error.what() << "\n";
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
