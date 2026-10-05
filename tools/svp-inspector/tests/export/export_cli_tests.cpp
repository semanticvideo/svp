// End-to-end tests of `svp-inspector export`, run against the real CLI.
//
// Usage: svp-inspector-export-cli-tests <svp-inspector> <fixture-dir> <case>

#include "export_test_support.hpp"
#include "svpi_fixture_builder.hpp"

#include "svp/package/embedded_svpi.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using export_test::CommandResult;
using export_test::TempDir;
using nlohmann::json;
namespace fs = std::filesystem;

struct Context {
  fs::path inspector;
  fs::path fixture_dir;
};

template <typename Value>
std::string le_bytes(const std::vector<Value>& values) {
  static_assert(std::endian::native == std::endian::little);
  std::string bytes(values.size() * sizeof(Value), '\0');
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

fs::path write_svpi(const fs::path& directory,
                    const std::vector<export_test::ZipEntrySpec>& entries,
                    const std::string& name = "fixture.svpi") {
  const auto path = directory / name;
  export_test::write_zip(path, entries);
  return path;
}

const json* find_layer(const json& summary, std::string_view entry) {
  for (const auto& layer : summary.at("layers")) {
    if (layer.at("entry") == entry) {
      return &layer;
    }
  }
  return nullptr;
}

const json* find_section(const json& summary, std::string_view name) {
  for (const auto& section : summary.at("sections")) {
    if (section.at("name") == name) {
      return &section;
    }
  }
  return nullptr;
}

void expect_refused(const CommandResult& result, int exit_code,
                    std::string_view error_code, const fs::path& out) {
  if (result.exit_code != exit_code) {
    std::cerr << result.stdout_text << "\n";
  }
  EXPORT_CHECK(result.exit_code == exit_code);
  const auto output = result.json();
  EXPORT_CHECK(output.at("status") == "error");
  EXPORT_CHECK(output.at("exit_code") == exit_code);
  EXPORT_CHECK(output.at("error").at("code") == error_code);
  EXPORT_CHECK(!fs::exists(out));
  EXPORT_CHECK(!export_test::has_export_leftovers(out.parent_path()));
}

void expect_exported(const CommandResult& result) {
  if (result.exit_code != 0) {
    std::cerr << result.stdout_text << "\n";
  }
  EXPORT_CHECK(result.exit_code == 0);
  EXPORT_CHECK(result.json().at("status") == "exported");
}

// Every package file entry becomes exactly one layer, in entry-name order.
void expect_every_entry_exported(const fs::path& package, const json& summary) {
  auto entries = export_test::zip_file_entries(package);
  std::ranges::sort(entries);
  std::vector<std::string> layers;
  for (const auto& layer : summary.at("layers")) {
    layers.push_back(layer.at("entry").get<std::string>());
  }
  EXPORT_CHECK(layers == entries);
}

// SVP packages are recognized by their mimetype and checked by the SVP
// validator. The repository's SVP fixtures predate the current RC2 validator,
// so a positive SVP export is covered by the real-package test instead.
void test_svp_detection(const Context& context) {
  TempDir temp{"svp"};
  const auto package = context.fixture_dir / "valid" / "dashboard_number.svp";
  const auto out = temp.path() / "export";
  const auto result = export_test::run_export(context.inspector, package, out);
  expect_refused(result, 3, "package_invalid", out);
  const auto output = result.json();
  const auto& validation = output.at("error").at("details").at("validation");
  EXPORT_CHECK(validation.at("status") == "invalid");
  EXPORT_CHECK(!validation.contains("embedding_transport"));

  // Content, not the file name, picks the SVP validator; that validator
  // still requires the .svp extension, exactly as svp-validator does.
  for (const auto* name : {"renamed.zip", "renamed.svpi"}) {
    const auto renamed = temp.path() / name;
    fs::copy_file(package, renamed);
    const auto renamed_result =
        export_test::run_export(context.inspector, renamed, out);
    expect_refused(renamed_result, 3, "package_invalid", out);
    const auto renamed_output = renamed_result.json();
    const auto& errors =
        renamed_output.at("error").at("details").at("validation").at("errors");
    EXPORT_CHECK(errors.size() == 1);
    EXPORT_CHECK(errors[0].at("code") == "X_VALIDATOR_INPUT_EXTENSION");
    EXPORT_CHECK(errors[0].at("message").get<std::string>().find(".svp ") !=
                 std::string::npos);
  }
}

void test_svpi_layers(const Context& context) {
  TempDir temp{"svpi"};
  const auto fixture = export_test::make_svpi_fixture();
  const auto package = write_svpi(temp.path(), fixture.entries);
  const auto out = temp.path() / "export";
  const auto result = export_test::run_export(context.inspector, package, out);
  expect_exported(result);
  EXPORT_CHECK(!export_test::has_export_leftovers(temp.path()));
  const auto layers = out / "layers";

  const auto summary = json::parse(export_test::read_file(out / "export.json"));
  EXPORT_CHECK(summary.at("export_format") == "svp-package-export");
  EXPORT_CHECK(summary.at("export_format_version") == 1);
  EXPORT_CHECK(summary.at("package").at("kind") == "svpi");
  EXPORT_CHECK(summary.at("package").at("format_versions").at("svpi_version") ==
               "0.1");
  EXPORT_CHECK(summary.at("package").at("embedded_transport").is_null());
  EXPORT_CHECK(summary.at("manifest") ==
               json::parse(export_test::zip_entry_content(package, "manifest.json")));
  EXPORT_CHECK(summary.at("media_binding") ==
               json::parse(export_test::zip_entry_content(package,
                                                          "media_binding.json")));
  EXPORT_CHECK(find_section(summary, "text")->at("declared_state") == "generated");
  EXPORT_CHECK(find_section(summary, "entities")->at("declared_state") ==
               "not_generated");
  EXPORT_CHECK(find_section(summary, "entities")->at("layer_count") == 0);
  EXPORT_CHECK(find_section(summary, "transcript")->at("declared_state").is_null());
  EXPORT_CHECK(summary.at("resolution_sources").at("frames").at("record_count") == 3);
  expect_every_entry_exported(package, summary);

  // Records keep their original text; svp_export is appended only where a
  // registered member is present.
  const auto observation_lines =
      export_test::read_lines(layers / "text" / "text_observations.jsonl");
  EXPORT_CHECK(observation_lines.size() == 4);
  const auto& first_original = export_test::entry_content(
      fixture.entries, "text/text_observations.jsonl");
  const auto first_record_text =
      first_original.substr(0, first_original.find('\n'));
  EXPORT_CHECK(observation_lines[0].starts_with(
      first_record_text.substr(0, first_record_text.size() - 1) +
      ",\"svp_export\":"));
  EXPORT_CHECK(observation_lines[2] ==
               "{\"raw_text\":\"no refs\",\"text_observation_id\":\"text_obs_000003\"}");
  const auto observations =
      export_test::read_jsonl(layers / "text" / "text_observations.jsonl");
  const auto& frames = observations[0].at("svp_export").at("source_frame_ids");
  EXPORT_CHECK(frames.size() == 3);
  EXPORT_CHECK(frames[0] == json({{"id", "frame_000002"},
                                  {"found", true},
                                  {"frame_index", 1},
                                  {"pts_us", 2000}}));
  EXPORT_CHECK(frames[1].at("pts_us") == 1000);
  EXPORT_CHECK(frames[2] == frames[0]);
  EXPORT_CHECK(observations[0].at("svp_export").at("evidence_crop_refs")[0] ==
               json({{"id", "crop_000001"},
                     {"found", true},
                     {"file", "layers/text/evidence_crops/crop_000001.jpg"}}));
  EXPORT_CHECK(observations[0].at("source_frame_ids") ==
               json({"frame_000002", "frame_000001", "frame_000002"}));
  EXPORT_CHECK(observations[1].at("svp_export").at("source_frame_ids")[0] ==
               json({{"id", "frame_999999"}, {"found", false}}));
  EXPORT_CHECK(!observations[2].contains("svp_export"));
  EXPORT_CHECK(!observations[3].at("svp_export").contains("source_frame_ids"));
  EXPORT_CHECK(observations[3].at("svp_export").at("evidence_crop_refs")[0].at(
                   "found") == false);
  EXPORT_CHECK(find_layer(summary, "text/text_observations.jsonl")
                   ->at("record_count") == 4);

  const auto crops =
      export_test::read_jsonl(layers / "text" / "evidence_crops.jsonl");
  EXPORT_CHECK(crops[0].at("svp_export").at("crop_file_path").at("file") ==
               "layers/text/evidence_crops/crop_000001.jpg");
  EXPORT_CHECK(crops[0].at("svp_export").at("source_frame_id").at("pts_us") == 2000);
  EXPORT_CHECK(crops[1].at("svp_export").at("crop_file_path").at("found") == false);
  EXPORT_CHECK(crops[1].at("svp_export").at("crop_file_path").at("file").is_null());
  EXPORT_CHECK(export_test::read_file(layers / "text" / "evidence_crops" /
                                      "crop_000001.jpg") == fixture.facts.crop_bytes);

  // Layers without rules are byte-identical record files.
  EXPORT_CHECK(export_test::read_file(layers / "transcript" / "words.jsonl") ==
               export_test::zip_entry_content(package, "transcript/words.jsonl"));

  // Depth: two verified blocks decoded to little-endian uint16.
  EXPORT_CHECK(export_test::read_file(layers / "spatial" /
                                      "depth.blocks.svpdz.decoded.bin") ==
               le_bytes(fixture.facts.depth_block_0) +
                   le_bytes(fixture.facts.depth_block_1));
  const auto blocks =
      export_test::read_jsonl(layers / "spatial" / "depth.blocks.svpdz.blocks.jsonl");
  EXPORT_CHECK(blocks.size() == 2);
  EXPORT_CHECK(blocks[1].at("block_offset") == fixture.facts.depth_block_1_offset);
  EXPORT_CHECK(blocks[1].at("decoded_offset") == 16);
  EXPORT_CHECK(blocks[1].at("dtype_name") == "uint16");
  EXPORT_CHECK(blocks[1].at("start_us") == 2000);
  EXPORT_CHECK(!fs::exists(layers / "spatial" / "depth.blocks.svpdz"));
  const auto depth_index =
      export_test::read_jsonl(layers / "spatial" / "depth.index.jsonl");
  const auto& resolved_block = depth_index[1].at("svp_export").at("block_file");
  EXPORT_CHECK(resolved_block.at("found") == true);
  EXPORT_CHECK(resolved_block.at("block_ordinal") == 1);
  EXPORT_CHECK(resolved_block.at("decoded_file") ==
               "layers/spatial/depth.blocks.svpdz.decoded.bin");
  EXPORT_CHECK(resolved_block.at("decoded_offset") == 16);
  EXPORT_CHECK(resolved_block.at("decoded_length") == 16);
  EXPORT_CHECK(resolved_block.at("extents") == json({4, 2, 1}));
  EXPORT_CHECK(depth_index[1].at("svp_export").at("frame_id").at("pts_us") == 2000);
  EXPORT_CHECK(depth_index[2].at("svp_export").at("block_file").at("found") ==
               false);

  // Embedding: float32 vector, little-endian, non-frame block fields kept.
  EXPORT_CHECK(export_test::read_file(layers / "embeddings" /
                                      "embeddings.blocks.svpez.decoded.bin") ==
               le_bytes(fixture.facts.embedding_vector));
  const auto embedding_blocks = export_test::read_jsonl(
      layers / "embeddings" / "embeddings.blocks.svpez.blocks.jsonl");
  EXPORT_CHECK(embedding_blocks[0].at("start_frame") ==
               std::numeric_limits<std::uint64_t>::max());
  EXPORT_CHECK(embedding_blocks[0].at("start_us") == -1);
  EXPORT_CHECK(embedding_blocks[0].at("dtype_name") == "float32");

  // Empty mask stream: empty files, layer state empty.
  const auto* masks = find_layer(summary, "spatial/masks.blocks.svpmz");
  EXPORT_CHECK(masks->at("representation") == "block_stream");
  EXPORT_CHECK(masks->at("record_count") == 0);
  EXPORT_CHECK(masks->at("state") == "empty");
  EXPORT_CHECK(fs::file_size(layers / "spatial" / "masks.blocks.svpmz.decoded.bin") == 0);

  // Unknown and binary entries are mirrored byte for byte.
  EXPORT_CHECK(find_layer(summary, "extensions/vendor.example/blob.bin")
                   ->at("representation") == "file");
  EXPORT_CHECK(export_test::read_file(layers / "extensions" / "vendor.example" /
                                      "blob.bin") == fixture.facts.extension_bytes);
  EXPORT_CHECK(export_test::read_file(layers / "manifest.json") ==
               export_test::zip_entry_content(package, "manifest.json"));
  EXPORT_CHECK(export_test::read_file(layers / "mimetype") ==
               "application/vnd.svp.interlace+zip");
}

void test_embedded_input(const Context& context) {
  TempDir temp{"embedded"};
  const auto fixture = export_test::make_svpi_fixture();
  const auto svpi = write_svpi(temp.path(), fixture.entries);
  const auto container = temp.path() / "clean.mp4";
  const auto embedded = temp.path() / "embedded.mp4";
  export_test::write_iso_bmff_container(container);
  EXPORT_CHECK(svp::package::embed_svpi_in_iso_bmff(container, svpi, embedded).success);

  const auto out = temp.path() / "embedded-export";
  const auto result = export_test::run_export(context.inspector, embedded, out);
  expect_exported(result);
  EXPORT_CHECK(result.json().at("package_kind") == "embedded_svpi");
  const auto summary = json::parse(export_test::read_file(out / "export.json"));
  EXPORT_CHECK(summary.at("package").at("kind") == "embedded_svpi");
  const auto& transport = summary.at("package").at("embedded_transport");
  EXPORT_CHECK(transport.at("profile_version") == 1);
  EXPORT_CHECK(transport.at("container_kind") == "mp4");
  EXPORT_CHECK(summary.at("package").at("format_versions").at(
                   "embedded_transport_profile_version") == 1);

  // The embedded package exports exactly like the same SVPI on its own.
  const auto plain_out = temp.path() / "plain-export";
  expect_exported(export_test::run_export(context.inspector, svpi, plain_out));
  EXPORT_CHECK(export_test::read_tree(out / "layers") ==
               export_test::read_tree(plain_out / "layers"));
}

void test_determinism(const Context& context) {
  TempDir temp{"determinism"};
  const auto fixture = export_test::make_svpi_fixture();
  const auto svpi = write_svpi(temp.path(), fixture.entries);
  const auto first = temp.path() / "first";
  const auto second = temp.path() / "nested" / "second";
  expect_exported(export_test::run_export(context.inspector, svpi, first));
  expect_exported(export_test::run_export(context.inspector, svpi, second));
  const auto first_tree = export_test::read_tree(first);
  EXPORT_CHECK(first_tree.size() > 20);
  EXPORT_CHECK(first_tree == export_test::read_tree(second));
}

void test_invalid_package(const Context& context) {
  TempDir temp{"invalid"};
  const auto out = temp.path() / "export";
  const auto result = export_test::run_export(
      context.inspector,
      context.fixture_dir / "invalid" / "invalid_text_reference.svp", out);
  expect_refused(result, 3, "package_invalid", out);
  EXPORT_CHECK(result.json().at("error").at("details").at("validation").at(
                   "status") == "invalid");
  EXPORT_CHECK(!result.json().at("error").at("details").at("validation").at(
                   "errors").empty());
}

void test_unsupported_version(const Context& context) {
  TempDir temp{"version"};
  for (const auto& [member, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"svpi_version", "0.2"}, {"svp_version", "2.0"}}) {
    auto fixture = export_test::make_svpi_fixture();
    auto manifest = json::parse(
        export_test::entry_content(fixture.entries, "manifest.json"));
    manifest[member] = value;
    export_test::set_entry(fixture.entries, "manifest.json", manifest.dump());
    const auto package = write_svpi(temp.path(), fixture.entries, member + ".svpi");
    const auto out = temp.path() / ("export-" + member);
    const auto result = export_test::run_export(context.inspector, package, out);
    expect_refused(result, 4, "unsupported_version", out);
    const auto output = result.json();
    const auto& details = output.at("error").at("details");
    EXPORT_CHECK(details.at("field") == member);
    EXPORT_CHECK(details.at("declared") == value);
  }

  auto fixture = export_test::make_svpi_fixture();
  auto manifest = json::parse(
        export_test::entry_content(fixture.entries, "manifest.json"));
  manifest.erase("svpi_version");
  export_test::set_entry(fixture.entries, "manifest.json", manifest.dump());
  const auto package = write_svpi(temp.path(), fixture.entries, "missing.svpi");
  const auto out = temp.path() / "export-missing";
  const auto result = export_test::run_export(context.inspector, package, out);
  expect_refused(result, 4, "unsupported_version", out);
  EXPORT_CHECK(result.json().at("error").at("details").at("declared").is_null());
}

void test_path_traversal(const Context& context) {
  TempDir temp{"traversal"};
  for (const auto& name : {std::string{"../escape.txt"},
                           std::string{"text/../../escape-nested.txt"}}) {
    auto fixture = export_test::make_svpi_fixture();
    export_test::add_entry(fixture.entries, name, "escaped");
    const auto package = write_svpi(temp.path(), fixture.entries, "traversal.svpi");
    const auto out = temp.path() / "work" / "export";
    fs::create_directories(out.parent_path());
    const auto result = export_test::run_export(context.inspector, package, out);
    EXPORT_CHECK(result.exit_code != 0);
    expect_refused(result, result.exit_code,
                   result.json().at("error").at("code").get<std::string>(), out);
    EXPORT_CHECK(result.exit_code == 3 || result.exit_code == 6);
    EXPORT_CHECK(!fs::exists(temp.path() / "escape.txt"));
    EXPORT_CHECK(!fs::exists(temp.path() / "escape-nested.txt"));
    EXPORT_CHECK(!fs::exists(temp.path() / "work" / "escape.txt"));
    EXPORT_CHECK(!fs::exists(temp.path() / "work" / "escape-nested.txt"));
    EXPORT_CHECK(fs::is_empty(out.parent_path()));
  }
}

void test_output_rules(const Context& context) {
  TempDir temp{"output"};
  const auto fixture = export_test::make_svpi_fixture();
  const auto package = write_svpi(temp.path(), fixture.entries);

  // An empty existing directory is used as is.
  const auto empty = temp.path() / "empty";
  fs::create_directories(empty);
  expect_exported(export_test::run_export(context.inspector, package, empty));
  EXPORT_CHECK(fs::exists(empty / "export.json"));

  // A previous export is kept without --overwrite and replaced with it.
  export_test::write_file(empty / "stale.txt", "stale");
  const auto refused = export_test::run_export(context.inspector, package, empty);
  EXPORT_CHECK(refused.exit_code == 5);
  EXPORT_CHECK(refused.json().at("error").at("code") == "output_exists");
  EXPORT_CHECK(fs::exists(empty / "stale.txt"));
  expect_exported(export_test::run_export(context.inspector, package, empty, true));
  EXPORT_CHECK(!fs::exists(empty / "stale.txt"));
  EXPORT_CHECK(fs::exists(empty / "export.json"));
  EXPORT_CHECK(!export_test::has_export_leftovers(temp.path()));

  // --overwrite never replaces a directory that is not a previous export.
  const auto unrelated = temp.path() / "unrelated";
  export_test::write_file(unrelated / "keep.txt", "keep");
  const auto not_replaceable =
      export_test::run_export(context.inspector, package, unrelated, true);
  EXPORT_CHECK(not_replaceable.exit_code == 5);
  EXPORT_CHECK(not_replaceable.json().at("error").at("code") ==
               "output_not_replaceable");
  EXPORT_CHECK(export_test::read_file(unrelated / "keep.txt") == "keep");
  EXPORT_CHECK(fs::directory_iterator(unrelated) != fs::directory_iterator());

  // Nor a directory that holds the package being exported.
  const auto holder = temp.path() / "holder";
  export_test::write_file(holder / "export.json", "{}");
  const auto inner = holder / "inner.svpi";
  fs::copy_file(package, inner);
  const auto inside = export_test::run_export(context.inspector, inner, holder, true);
  EXPORT_CHECK(inside.exit_code == 5);
  EXPORT_CHECK(inside.json().at("error").at("code") == "output_not_replaceable");
  EXPORT_CHECK(fs::exists(inner));
  EXPORT_CHECK(export_test::read_file(holder / "export.json") == "{}");

  // Nor a regular file.
  const auto file_target = temp.path() / "file-target";
  export_test::write_file(file_target, "file");
  EXPORT_CHECK(export_test::run_export(context.inspector, package, file_target, true)
                   .exit_code == 5);
  EXPORT_CHECK(export_test::read_file(file_target) == "file");
  EXPORT_CHECK(!export_test::has_export_leftovers(temp.path()));
}

void test_malformed_content(const Context& context) {
  TempDir temp{"malformed"};
  struct Case {
    std::string entry;
    std::string content;
    int exit_code;
    std::string error_code;
  };
  const std::vector<Case> cases{
      {"text/text_observations.jsonl",
       "{\"text_observation_id\":\"text_obs_000001\"}\nnot json\n", 6,
       "malformed_record"},
      {"text/text_observations.jsonl", "[1,2]\n", 6, "malformed_record"},
      {"transcript/words.jsonl",
       "{\"id\":\"word_000000\",\"svp_export\":{}}\n", 6,
       "reserved_member_present"},
      {"colors/color_summary.json", "{\"broken\":", 6, "malformed_document"},
  };
  for (const auto& test_case : cases) {
    auto fixture = export_test::make_svpi_fixture();
    export_test::set_entry(fixture.entries, test_case.entry, test_case.content);
    const auto package = write_svpi(temp.path(), fixture.entries, "malformed.svpi");
    const auto out = temp.path() / "export";
    const auto result = export_test::run_export(context.inspector, package, out);
    expect_refused(result, test_case.exit_code, test_case.error_code, out);
    EXPORT_CHECK(result.json().at("error").at("details").at("entry") ==
                 test_case.entry);
  }

  // A corrupted depth payload fails block verification.
  auto fixture = export_test::make_svpi_fixture();
  auto depth = std::ranges::find(fixture.entries, std::string{"spatial/depth.blocks.svpdz"},
                                 &export_test::ZipEntrySpec::name);
  depth->content.back() = static_cast<char>(depth->content.back() ^ 0x5a);
  const auto package = write_svpi(temp.path(), fixture.entries, "corrupt.svpi");
  const auto out = temp.path() / "export";
  const auto result = export_test::run_export(context.inspector, package, out);
  expect_refused(result, 6, "invalid_block_stream", out);
}

void test_unreadable_input(const Context& context) {
  TempDir temp{"input"};
  const auto out = temp.path() / "export";
  const auto text = temp.path() / "notes.svpi";
  export_test::write_file(text, "this is not a package");
  expect_refused(export_test::run_export(context.inspector, text, out), 2,
                 "input_unrecognized", out);
  expect_refused(
      export_test::run_export(context.inspector, temp.path() / "missing.svp", out),
      2, "input_missing", out);
  expect_refused(export_test::run_export(context.inspector, temp.path(), out), 2,
                 "input_not_regular_file", out);
}

std::set<std::string> keys_of(const json& object) {
  std::set<std::string> keys;
  for (const auto& [key, value] : object.items()) {
    keys.insert(key);
  }
  return keys;
}

std::set<std::string> required_of(const json& schema) {
  std::set<std::string> keys;
  for (const auto& name : schema.at("required")) {
    keys.insert(name.get<std::string>());
  }
  return keys;
}

json load_schema(std::string_view file) {
  const auto schema = json::parse(
      export_test::read_file(fs::path{SVP_EXPORT_SCHEMA_DIR} / std::string{file}));
  EXPORT_CHECK(schema.at("$schema") ==
               "https://json-schema.org/draft/2020-12/schema");
  EXPORT_CHECK(!schema.at("examples").empty());
  return schema;
}

// The published schemas name exactly the members the export writes.
void test_schema_conformance(const Context& context) {
  TempDir temp{"schemas"};
  const auto fixture = export_test::make_svpi_fixture();
  const auto package = write_svpi(temp.path(), fixture.entries);
  const auto out = temp.path() / "export";
  const auto result = export_test::run_export(context.inspector, package, out);
  expect_exported(result);

  const auto result_schema = load_schema("package-export-result.schema.json");
  EXPORT_CHECK(keys_of(result.json()) ==
               required_of(result_schema.at("oneOf")[0]));
  const auto failure = export_test::run_export(
      context.inspector, temp.path() / "missing.svpi", temp.path() / "none");
  EXPORT_CHECK(keys_of(failure.json()) ==
               required_of(result_schema.at("oneOf")[1]));
  EXPORT_CHECK(keys_of(failure.json().at("error")) ==
               required_of(result_schema.at("oneOf")[1].at("properties").at("error")));

  const auto summary_schema = load_schema("package-export.schema.json");
  const auto summary = json::parse(export_test::read_file(out / "export.json"));
  const auto& properties = summary_schema.at("properties");
  EXPORT_CHECK(keys_of(summary) == required_of(summary_schema));
  EXPORT_CHECK(keys_of(summary.at("package")) == required_of(properties.at("package")));
  EXPORT_CHECK(keys_of(summary.at("validation")) ==
               required_of(properties.at("validation")));
  EXPORT_CHECK(keys_of(summary.at("sections")[0]) ==
               required_of(properties.at("sections").at("items")));
  EXPORT_CHECK(keys_of(summary.at("resolution_sources").at("frames")) ==
               required_of(summary_schema.at("$defs").at("resolution_source")));
  for (const auto& layer : summary.at("layers")) {
    EXPORT_CHECK(keys_of(layer) ==
                 required_of(summary_schema.at("$defs").at("layer")));
  }

  const auto block_schema = load_schema("package-export-block.schema.json");
  for (const auto& block : export_test::read_jsonl(
           out / "layers" / "spatial" / "depth.blocks.svpdz.blocks.jsonl")) {
    EXPORT_CHECK(keys_of(block) == required_of(block_schema));
  }

  const auto record_schema = load_schema("package-export-record.schema.json");
  const auto& definitions = record_schema.at("$defs");
  const auto depth_index =
      export_test::read_jsonl(out / "layers" / "spatial" / "depth.index.jsonl");
  const auto& found_block = depth_index[0].at("svp_export").at("block_file");
  auto block_keys = required_of(definitions.at("block_resolution"));
  const auto block_then = required_of(definitions.at("block_resolution").at("then"));
  block_keys.insert(block_then.begin(), block_then.end());
  EXPORT_CHECK(keys_of(found_block) == block_keys);
  const auto& frame = depth_index[0].at("svp_export").at("frame_id");
  auto frame_keys = required_of(definitions.at("frame_resolution"));
  const auto frame_then = required_of(definitions.at("frame_resolution").at("then"));
  frame_keys.insert(frame_then.begin(), frame_then.end());
  EXPORT_CHECK(keys_of(frame) == frame_keys);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: svp-inspector-export-cli-tests <svp-inspector> "
                 "<fixture-dir> <case>\n";
    return 2;
  }
  const Context context{argv[1], argv[2]};
  const std::map<std::string, std::function<void(const Context&)>> cases{
      {"svp-detection", test_svp_detection},
      {"svpi-layers", test_svpi_layers},
      {"embedded-input", test_embedded_input},
      {"determinism", test_determinism},
      {"invalid-package", test_invalid_package},
      {"unsupported-version", test_unsupported_version},
      {"path-traversal", test_path_traversal},
      {"output-rules", test_output_rules},
      {"malformed-content", test_malformed_content},
      {"unreadable-input", test_unreadable_input},
      {"schema-conformance", test_schema_conformance},
  };
  const auto found = cases.find(argv[3]);
  if (found == cases.end()) {
    std::cerr << "unknown case: " << argv[3] << "\n";
    return 2;
  }
  try {
    found->second(context);
  } catch (const std::exception& error) {
    std::cerr << argv[3] << ": " << error.what() << "\n";
    return 1;
  }
  std::cout << argv[3] << ": passed\n";
  return 0;
}
