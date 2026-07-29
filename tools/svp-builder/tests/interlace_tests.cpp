#include "svp/builder/interlace.hpp"
#include "svp/builder/build_progress.hpp"
#include "model_cache_test_fixture.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/validator.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"          \
                << __LINE__ << "\n";                                         \
      std::abort();                                                          \
    }                                                                        \
  } while (0)

std::string make_utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

std::filesystem::path make_test_dir(const std::string& name) {
  const auto root = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void create_mock_source_media(const std::filesystem::path& path, int size_bytes = 1024) {
  std::ofstream out(path, std::ios::binary);
  for (int i = 0; i < size_bytes; ++i) {
    out.put(static_cast<char>(i % 256));
  }
}

nlohmann::json make_svpi_manifest(const std::string& package_id) {
  return {
    {"format", "svpi"},
    {"svpi_version", std::string{svp::package::kSvpiVersion}},
    {"svp_version", "1.0-rc.2"},
    {"package_id", package_id},
    {"created_utc", make_utc_timestamp()},
    {"media_binding_ref", "media_binding.json"},
    {"primary_media_binding_id", "mb_primary_000001"},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }},
    {"sections", nlohmann::json::object({
      {"transcript", {{"state", "not_generated"}}},
      {"timeline", {{"state", "not_generated"}}},
      {"text", {{"state", "not_generated"}}},
      {"colors", {{"state", "not_generated"}}},
      {"entities", {{"state", "not_generated"}}},
      {"spatial", {{"state", "not_generated"}}},
      {"relationships", {{"state", "not_generated"}}},
      {"embeddings", {{"state", "not_generated"}}}
    })}
  };
}

svp::package::MediaBindingDocument make_mock_binding(
    const std::filesystem::path& source_path,
    bool with_blake3 = true) {
  svp::package::MediaBinding binding;
  binding.binding_id = "mb_primary_000001";
  binding.media_role = "primary_source";
  binding.media_id = "media_src_000001";
  binding.binding_contract = std::string{svp::package::kSvpiBindingContract};
  binding.verification_state = "pending";
  binding.size_bytes = 0;
  binding.duration_us = 0;
  binding.container_format = "unknown";
  binding.location_hints.original_filename = source_path.filename().string();

  if (with_blake3 && std::filesystem::exists(source_path)) {
    svp::package::MediaBindingFactoryOptions opts;
    opts.compute_full_blake3 = true;
    opts.compute_chunk_proof = false;
    auto doc = svp::package::create_media_binding(source_path, opts);
    if (!doc.bindings.empty()) {
      binding.identity = doc.bindings[0].identity;
      binding.size_bytes = doc.bindings[0].size_bytes;
    }
  } else {
    binding.identity.blake3_state = svp::package::Blake3State::pending;
    binding.identity.blake3_state_reason = "BLAKE3 not computed in test";
  }

  svp::package::MediaBindingDocument doc;
  doc.primary_binding_id = "mb_primary_000001";
  doc.bindings.push_back(std::move(binding));
  return doc;
}

bool build_minimal_svpi(
    const std::filesystem::path& svpi_path,
    const std::filesystem::path& source_path,
    bool with_blake3 = true) {
  const auto root = svpi_path.parent_path();
  auto staging = root / "staging";
  std::filesystem::remove_all(staging);
  std::filesystem::create_directories(staging);
  std::filesystem::create_directories(staging / "provenance");
  std::filesystem::create_directories(staging / "index");

  write_jsonl(staging / "provenance" / "processors.jsonl", {
    nlohmann::json{
      {"processor_id", "proc_test_000001"},
      {"processor_name", "test"},
      {"processor_version", "1.0.0"},
      {"stage", "test"},
      {"ran_utc", make_utc_timestamp()}
    }
  });

  write_jsonl(staging / "provenance" / "interlace_events.jsonl", {
    nlohmann::json{
      {"event_id", "evt_test_000001"},
      {"event_type", "test"},
      {"timestamp_utc", make_utc_timestamp()}
    }
  });

  auto manifest = make_svpi_manifest("svpi_test_pkg");
  auto binding = make_mock_binding(source_path, with_blake3);

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging, manifest_copy)) {
    return false;
  }

  return svp::package::write_svpi_package(svpi_path, staging, manifest, binding);
}

bool build_minimal_svp(
    const std::filesystem::path& svp_path,
    const std::filesystem::path& source_path) {
  const auto root = svp_path.parent_path();
  auto staging = root / "svp_staging";
  std::filesystem::remove_all(staging);
  std::filesystem::create_directories(staging);
  std::filesystem::create_directories(staging / "provenance");

  write_jsonl(staging / "provenance" / "processors.jsonl", {
    nlohmann::json{
      {"processor_id", "proc_svp_test_000001"},
      {"processor_name", "test"},
      {"processor_version", "1.0.0"},
      {"stage", "test"},
      {"ran_utc", make_utc_timestamp()}
    }
  });

  write_jsonl(staging / "provenance" / "interlace_events.jsonl", {
    nlohmann::json{
      {"event_id", "evt_svp_build_000001"},
      {"event_type", "svpi_created_from_media"},
      {"event_utc", make_utc_timestamp()},
      {"authority", "builder_derived"}
    }
  });

  nlohmann::json manifest = {
    {"svp_version", "1.0-rc.2"},
    {"package_id", "svp_test_pkg"},
    {"created_utc", make_utc_timestamp()},
    {"primary_media_id", "media_000001"},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }}
  };

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging, manifest_copy)) {
    return false;
  }

  return svp::package::write_package_skeleton(svp_path, staging, source_path, manifest);
}

bool zip_has_entry(const std::filesystem::path& zip_path, const std::string& entry) {
  int error_code = ZIP_ER_OK;
  zip_t* archive = zip_open(zip_path.string().c_str(), ZIP_RDONLY, &error_code);
  if (!archive) return false;

  zip_stat_t stat;
  zip_stat_init(&stat);
  bool found = (zip_stat(archive, entry.c_str(), 0, &stat) == 0);
  zip_discard(archive);
  return found;
}

bool zip_has_media_original(const std::filesystem::path& zip_path) {
  int error_code = ZIP_ER_OK;
  zip_t* archive = zip_open(zip_path.string().c_str(), ZIP_RDONLY, &error_code);
  if (!archive) return false;

  const auto num = zip_get_num_entries(archive, 0);
  bool found = false;
  for (zip_int64_t i = 0; i < num; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0) continue;
    if (stat.name == nullptr) continue;
    std::string name(stat.name);
    if (name.rfind("media/original/", 0) == 0) {
      found = true;
      break;
    }
  }
  zip_discard(archive);
  return found;
}

void test_media_binding_factory_creates_binding_with_blake3() {
  auto root = make_test_dir("svp-phase2-binding-factory-test");
  auto source = root / "test.mov";
  create_mock_source_media(source, 2048);

  svp::package::MediaBindingFactoryOptions opts;
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = true;
  opts.chunk_size_bytes = 512;

  auto doc = svp::package::create_media_binding(source, opts);
  CHECK(doc.bindings.size() == 1);
  CHECK(doc.primary_binding_id == "mb_primary_000001");
  CHECK(doc.bindings[0].size_bytes == 2048);
  CHECK(doc.bindings[0].identity.blake3_state == svp::package::Blake3State::present);
  CHECK(!doc.bindings[0].identity.blake3_hash.empty());
  CHECK(doc.bindings[0].identity.chunk_proof.has_value());
  CHECK(doc.bindings[0].identity.chunk_proof->chunk_count > 0);

  std::filesystem::remove_all(root);
  std::cout << "  test_media_binding_factory_creates_binding_with_blake3 passed\n";
}

void test_media_binding_verification_matching() {
  auto root = make_test_dir("svp-phase2-binding-verify-match");
  auto source = root / "test.mov";
  create_mock_source_media(source, 4096);

  svp::package::MediaBindingFactoryOptions opts;
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = false;

  auto doc = svp::package::create_media_binding(source, opts);
  auto result = svp::package::verify_media_binding(source, doc);

  CHECK(result.state == svp::package::BindingVerificationState::verified);
  CHECK(result.state_label == "verified");

  std::filesystem::remove_all(root);
  std::cout << "  test_media_binding_verification_matching passed\n";
}

void test_media_binding_verification_wrong_file() {
  auto root = make_test_dir("svp-phase2-binding-verify-mismatch");
  auto source = root / "test.mov";
  auto wrong = root / "wrong.mov";
  create_mock_source_media(source, 4096);
  create_mock_source_media(wrong, 2048);

  svp::package::MediaBindingFactoryOptions opts;
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = false;

  auto doc = svp::package::create_media_binding(source, opts);
  auto result = svp::package::verify_media_binding(wrong, doc);

  CHECK(result.state == svp::package::BindingVerificationState::mismatch);
  CHECK(result.state_label == "mismatch");

  std::filesystem::remove_all(root);
  std::cout << "  test_media_binding_verification_wrong_file passed\n";
}

void test_interlace_create_produces_valid_svpi() {
  auto root = make_test_dir("svp-phase2-create-test");
  auto source = root / "test.mov";
  create_mock_source_media(source, 1024);
  auto svpi_path = root / "output.svpi";

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source.string();
  opts.output_path = svpi_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = false;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svpi_path));

  auto layout_result = svp::package::read_package_layout(svpi_path);
  CHECK(layout_result.has_value());
  const auto& layout = layout_result.value();
  CHECK(layout.has_entry("mimetype"));
  CHECK(layout.has_entry("manifest.json"));
  CHECK(layout.has_entry("media_binding.json"));
  CHECK(layout.has_entry("index/index.sqlite"));
  CHECK(layout.has_entry("index/index_manifest.json"));
  CHECK(layout.has_entry("provenance/processors.jsonl"));
  CHECK(layout.has_entry("provenance/interlace_events.jsonl"));
  CHECK(!zip_has_media_original(svpi_path));

  auto manifest_entry = svp::package::read_package_entry(svpi_path, "manifest.json");
  CHECK(manifest_entry.has_value());
  auto manifest_json = nlohmann::json::parse(manifest_entry.value(), nullptr, false);
  CHECK(manifest_json.is_object());
  CHECK(manifest_json.contains("sections"));
  for (const auto& key : {"transcript", "timeline", "text", "colors",
                          "entities", "spatial", "relationships", "embeddings"}) {
    CHECK(manifest_json["sections"][key]["state"] == "not_generated");
  }

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_produces_valid_svpi passed\n";
}

void test_interlace_create_falls_back_on_pipeline_failure() {
  auto root = make_test_dir("svp-phase2-create-fallback");
  auto source = root / "test.mov";
  create_mock_source_media(source, 1024);
  auto svpi_path = root / "output.svpi";

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source.string();
  opts.output_path = svpi_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = false;
  opts.model_cache_dir = svp::builder::test::write_valid_model_cache(
                             root / "model-cache").string();

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svpi_path));

  auto manifest_entry = svp::package::read_package_entry(svpi_path, "manifest.json");
  CHECK(manifest_entry.has_value());
  auto manifest_json = nlohmann::json::parse(manifest_entry.value(), nullptr, false);
  CHECK(manifest_json.is_object());
  CHECK(manifest_json.contains("sections"));
  for (const auto& key : {"transcript", "timeline", "text", "colors",
                          "entities", "spatial", "relationships", "embeddings"}) {
    CHECK(manifest_json["sections"][key]["state"] == "blocked");
  }

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_falls_back_on_pipeline_failure passed\n";
}

void test_interlace_validate_structure_only() {
  auto root = make_test_dir("svp-phase2-validate-structure");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "test.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, false));

  svp::builder::InterlaceValidateOptions opts;
  opts.svpi_path = svpi_path.string();

  auto result = svp::builder::interlace_validate(opts);
  CHECK(result.structure_valid);
  CHECK(!result.binding_attempted);

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_validate_structure_only passed\n";
}

void test_interlace_validate_with_matching_media() {
  auto root = make_test_dir("svp-phase2-validate-match");
  auto source = root / "test.mov";
  create_mock_source_media(source, 1024);
  auto svpi_path = root / "test.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceValidateOptions opts;
  opts.svpi_path = svpi_path.string();
  opts.media_path = source.string();

  auto result = svp::builder::interlace_validate(opts);
  CHECK(result.structure_valid);
  CHECK(result.binding_attempted);
  CHECK(result.binding_verified);
  CHECK(result.binding_state_label == "verified");

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_validate_with_matching_media passed\n";
}

void test_interlace_validate_with_wrong_media() {
  auto root = make_test_dir("svp-phase2-validate-mismatch");
  auto source = root / "test.mov";
  auto wrong = root / "wrong.mov";
  create_mock_source_media(source, 1024);
  create_mock_source_media(wrong, 512);
  auto svpi_path = root / "test.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceValidateOptions opts;
  opts.svpi_path = svpi_path.string();
  opts.media_path = wrong.string();

  auto result = svp::builder::interlace_validate(opts);
  CHECK(result.binding_attempted);
  CHECK(!result.binding_verified);
  CHECK(result.binding_state_label == "mismatch");

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_validate_with_wrong_media passed\n";
}

void test_interlace_inspect_shows_svpi_fields() {
  auto root = make_test_dir("svp-phase2-inspect-test");
  auto source = root / "test.mov";
  create_mock_source_media(source, 256);
  auto svpi_path = root / "test.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceInspectOptions opts;
  opts.svpi_path = svpi_path.string();

  auto result = svp::builder::interlace_inspect(opts);
  CHECK(result.success);
  CHECK(result.artifact_type == "SVPI");
  CHECK(result.has_index_sqlite);
  CHECK(result.has_index_manifest);
  CHECK(result.has_provenance);
  CHECK(!result.has_media_original);
  CHECK(result.recombination_ready);

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_inspect_shows_svpi_fields passed\n";
}

void test_interlace_extract_from_svp() {
  auto root = make_test_dir("svp-phase2-extract-test");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svp_path = root / "test.svp";
  auto out_dir = root / "extracted";

  CHECK(build_minimal_svp(svp_path, source));
  CHECK(zip_has_entry(svp_path, "media/original/source_000.mov"));

  svp::builder::InterlaceExtractOptions opts;
  opts.svp_path = svp_path.string();
  opts.out_dir = out_dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_extract(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(result.extracted_media_path));
  CHECK(std::filesystem::exists(result.extracted_svpi_path));
  CHECK(!std::filesystem::exists(
      std::filesystem::path(result.extracted_svpi_path.string() + ".staging")));
  for (const auto& entry : std::filesystem::directory_iterator(out_dir)) {
    CHECK(entry.path().filename().string().find(".staging") == std::string::npos);
  }

  auto layout_result = svp::package::read_package_layout(result.extracted_svpi_path);
  CHECK(layout_result.has_value());
  const auto& layout = layout_result.value();
  CHECK(layout.has_entry("media_binding.json"));
  CHECK(!zip_has_media_original(result.extracted_svpi_path));

  auto binding_entry = svp::package::read_package_entry(
      result.extracted_svpi_path, "media_binding.json");
  CHECK(binding_entry.has_value());

  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  CHECK(!binding_doc.bindings.empty());

  auto verification = svp::package::verify_media_binding(
      result.extracted_media_path, binding_doc);
  CHECK(verification.state == svp::package::BindingVerificationState::verified);

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_extract_from_svp passed\n";
}

void test_interlace_recombine_produces_valid_svp() {
  auto root = make_test_dir("svp-phase2-recombine-test");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "test.svpi";
  auto svp_path = root / "recombined.svp";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceRecombineOptions opts;
  opts.media_path = source.string();
  opts.svpi_path = svpi_path.string();
  opts.output_path = svp_path.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_recombine(opts);
  CHECK(result.success);
  CHECK(result.binding_verified);
  CHECK(std::filesystem::exists(svp_path));
  CHECK(zip_has_entry(svp_path, "media/original/source_000.mov"));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_recombine_produces_valid_svp passed\n";
}

void test_interlace_recombine_fails_on_wrong_media() {
  auto root = make_test_dir("svp-phase2-recombine-wrong");
  auto source = root / "test.mov";
  auto wrong = root / "wrong.mov";
  create_mock_source_media(source, 512);
  create_mock_source_media(wrong, 256);
  auto svpi_path = root / "test.svpi";
  auto svp_path = root / "recombined.svp";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceRecombineOptions opts;
  opts.media_path = wrong.string();
  opts.svpi_path = svpi_path.string();
  opts.output_path = svp_path.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_recombine(opts);
  CHECK(!result.success);
  CHECK(!result.binding_verified);
  CHECK(!std::filesystem::exists(svp_path));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_recombine_fails_on_wrong_media passed\n";
}

void test_filename_only_matching_does_not_pass_binding() {
  auto root = make_test_dir("svp-phase2-filename-only");
  auto source = root / "test.mov";
  auto renamed = root / "renamed.mov";
  create_mock_source_media(source, 1024);
  create_mock_source_media(renamed, 512);
  auto svpi_path = root / "test.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceValidateOptions opts;
  opts.svpi_path = svpi_path.string();
  opts.media_path = renamed.string();

  auto result = svp::builder::interlace_validate(opts);
  CHECK(result.binding_attempted);
  CHECK(!result.binding_verified);
  CHECK(result.binding_state_label == "mismatch");

  std::filesystem::remove_all(root);
  std::cout << "  test_filename_only_matching_does_not_pass_binding passed\n";
}

void test_missing_svpi_index_fails_validation() {
  auto root = make_test_dir("svp-phase2-missing-index");
  auto source = root / "test.mov";
  create_mock_source_media(source, 256);
  auto svpi_path = root / "test.svpi";
  auto corrupted_path = root / "corrupted.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, false));

  int error_code = ZIP_ER_OK;
  zip_t* src_archive = zip_open(svpi_path.string().c_str(), ZIP_RDONLY, &error_code);
  CHECK(src_archive != nullptr);

  zip_t* dst_archive = zip_open(corrupted_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error_code);
  CHECK(dst_archive != nullptr);

  const auto num = zip_get_num_entries(src_archive, 0);
  for (zip_int64_t i = 0; i < num; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(src_archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0) continue;
    if (stat.name == nullptr) continue;
    std::string name(stat.name);

    if (name.rfind("index/", 0) == 0) {
      continue;
    }

    zip_source_t* source_entry = zip_source_zip(dst_archive, src_archive,
        static_cast<zip_uint64_t>(i), 0, 0, -1);
    if (source_entry) {
      zip_file_add(dst_archive, name.c_str(), source_entry, ZIP_FL_OVERWRITE);
    }
  }

  CHECK(zip_close(dst_archive) == 0);
  zip_discard(src_archive);

  svp::validation::SvpiValidatorOptions vopts;
  auto report = svp::validation::validate_svpi_package(corrupted_path, vopts);
  CHECK(svp::validation::exit_code(report) != 0);

  bool has_index_error = false;
  for (const auto& err : report.errors) {
    if (err.code == "ERR_SVPI_MISSING_INDEX") {
      has_index_error = true;
      break;
    }
  }
  CHECK(has_index_error);

  std::filesystem::remove_all(root);
  std::cout << "  test_missing_svpi_index_fails_validation passed\n";
}

void test_media_original_in_svpi_fails_validation() {
  auto root = make_test_dir("svp-phase2-forbidden-media");
  auto source = root / "test.mov";
  create_mock_source_media(source, 256);
  auto svpi_path = root / "test.svpi";
  auto corrupted_path = root / "corrupted.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, false));

  int error_code = ZIP_ER_OK;
  zip_t* archive = zip_open(corrupted_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error_code);
  CHECK(archive != nullptr);

  zip_t* src = zip_open(svpi_path.string().c_str(), ZIP_RDONLY, &error_code);
  CHECK(src != nullptr);

  const auto num = zip_get_num_entries(src, 0);
  for (zip_int64_t i = 0; i < num; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(src, static_cast<zip_uint64_t>(i), 0, &stat) != 0) continue;
    if (stat.name == nullptr) continue;
    std::string name(stat.name);
    zip_source_t* s = zip_source_zip(archive, src, static_cast<zip_uint64_t>(i), 0, 0, -1);
    if (s) {
      zip_file_add(archive, name.c_str(), s, ZIP_FL_OVERWRITE);
    }
  }

  std::string forbidden_content = "forbidden media content";
  zip_source_t* forbidden_source = zip_source_buffer(archive, forbidden_content.data(),
      forbidden_content.size(), 0);
  CHECK(forbidden_source != nullptr);
  zip_file_add(archive, "media/original/source_000.mov", forbidden_source, ZIP_FL_OVERWRITE);

  CHECK(zip_close(archive) == 0);
  zip_discard(src);

  svp::validation::SvpiValidatorOptions vopts;
  auto report = svp::validation::validate_svpi_package(corrupted_path, vopts);
  CHECK(svp::validation::exit_code(report) != 0);

  bool has_forbidden_error = false;
  for (const auto& err : report.errors) {
    if (err.code == "ERR_SVPI_FORBIDDEN_PRIMARY_MEDIA") {
      has_forbidden_error = true;
      break;
    }
  }
  CHECK(has_forbidden_error);

  std::filesystem::remove_all(root);
  std::cout << "  test_media_original_in_svpi_fails_validation passed\n";
}

void test_binding_verification_fails_on_unavailable_blake3() {
  auto root = make_test_dir("svp-phase2-unavailable-blake3");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);

  svp::package::MediaBinding binding;
  binding.binding_id = "mb_primary_000001";
  binding.media_role = "primary_source";
  binding.media_id = "media_src_000001";
  binding.binding_contract = std::string{svp::package::kSvpiBindingContract};
  binding.verification_state = "pending";
  binding.size_bytes = 512;
  binding.identity.blake3_state = svp::package::Blake3State::unavailable;
  binding.identity.blake3_state_reason = "BLAKE3 not computed";

  svp::package::MediaBindingDocument doc;
  doc.primary_binding_id = "mb_primary_000001";
  doc.bindings.push_back(std::move(binding));

  auto result = svp::package::verify_media_binding(source, doc);
  CHECK(result.state == svp::package::BindingVerificationState::unavailable);
  CHECK(result.state_label == "unavailable");

  std::filesystem::remove_all(root);
  std::cout << "  test_binding_verification_fails_on_unavailable_blake3 passed\n";
}

void test_binding_verification_includes_container_and_streams() {
  auto root = make_test_dir("svp-phase2-container-streams");
  auto source = root / "test.mov";
  create_mock_source_media(source, 1024);

  svp::package::MediaBindingFactoryOptions opts;
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = true;
  opts.chunk_size_bytes = 512;

  auto doc = svp::package::create_media_binding(source, opts);
  auto result = svp::package::verify_media_binding(source, doc);

  CHECK(result.state == svp::package::BindingVerificationState::verified);
  bool has_chunk_proof_check = false;
  for (const auto& check : result.passing_checks) {
    if (check == "chunk_proof") has_chunk_proof_check = true;
  }
  CHECK(has_chunk_proof_check);

  std::filesystem::remove_all(root);
  std::cout << "  test_binding_verification_includes_container_and_streams passed\n";
}

void test_validator_rejects_binding_without_chunk_hashes() {
  auto root = make_test_dir("svp-phase2-no-chunk-hashes");
  auto source = root / "test.mov";
  create_mock_source_media(source, 256);
  auto svpi_path = root / "test.svpi";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
  CHECK(binding_entry.has_value());
  auto binding_json = nlohmann::json::parse(binding_entry.value(), nullptr, false);
  CHECK(binding_json.is_object());

  if (binding_json.contains("bindings") && binding_json["bindings"].is_array() &&
      !binding_json["bindings"].empty()) {
    binding_json["bindings"][0].erase("identity");
    binding_json["bindings"][0]["identity"] = {
      {"full_file_blake3", {{"state", "present"}, {"value", "blake3:dummy"}}}
    };
  }

  auto corrupted_path = root / "corrupted.svpi";
  int error_code = ZIP_ER_OK;
  zip_t* src = zip_open(svpi_path.string().c_str(), ZIP_RDONLY, &error_code);
  CHECK(src != nullptr);
  zip_t* dst = zip_open(corrupted_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error_code);
  CHECK(dst != nullptr);

  const auto num = zip_get_num_entries(src, 0);
  for (zip_int64_t i = 0; i < num; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(src, static_cast<zip_uint64_t>(i), 0, &stat) != 0) continue;
    if (stat.name == nullptr) continue;
    std::string name(stat.name);
    if (name == "media_binding.json") {
      std::string replaced = binding_json.dump(2) + "\n";
      char* buf = static_cast<char*>(malloc(replaced.size()));
      std::memcpy(buf, replaced.data(), replaced.size());
      zip_source_t* s = zip_source_buffer(dst, buf, replaced.size(), 1);
      if (s) zip_file_add(dst, name.c_str(), s, ZIP_FL_OVERWRITE);
    } else {
      zip_source_t* s = zip_source_zip(dst, src, static_cast<zip_uint64_t>(i), 0, 0, -1);
      if (s) zip_file_add(dst, name.c_str(), s, ZIP_FL_OVERWRITE);
    }
  }
  CHECK(zip_close(dst) == 0);
  zip_discard(src);

  svp::validation::SvpiValidatorOptions vopts;
  auto report = svp::validation::validate_svpi_package(corrupted_path, vopts);
  CHECK(svp::validation::exit_code(report) != 0);

  bool has_chunk_error = false;
  for (const auto& err : report.errors) {
    if (err.message.find("chunk_hashes") != std::string::npos) {
      has_chunk_error = true;
      break;
    }
  }
  CHECK(has_chunk_error);

  std::filesystem::remove_all(root);
  std::cout << "  test_validator_rejects_binding_without_chunk_hashes passed\n";
}

void test_extract_preserves_existing_provenance_events() {
  auto root = make_test_dir("svp-phase2-provenance-preserve");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svp_path = root / "test.svp";
  auto out_dir = root / "extracted";

  CHECK(build_minimal_svp(svp_path, source));

  svp::builder::InterlaceExtractOptions opts;
  opts.svp_path = svp_path.string();
  opts.out_dir = out_dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_extract(opts);
  CHECK(result.success);

  auto events_entry = svp::package::read_package_entry(
      result.extracted_svpi_path, "provenance/interlace_events.jsonl");
  CHECK(events_entry.has_value());

  int event_count = 0;
  bool has_extract_event = false;
  std::istringstream stream(events_entry.value());
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      event_count++;
      auto j = nlohmann::json::parse(line, nullptr, false);
      if (!j.is_discarded() && j.contains("event_type") &&
          j["event_type"] == "svpi_extracted_from_svp") {
        has_extract_event = true;
      }
    }
  }
  CHECK(event_count >= 2);
  CHECK(has_extract_event);

  std::filesystem::remove_all(root);
  std::cout << "  test_extract_preserves_existing_provenance_events passed\n";
}

}  // namespace

// --- Progress event tests ---

namespace {

class CapturingProgressSink : public svp::builder::BuildProgressSink {
 public:
  void emit(const svp::builder::ProgressEvent& event) override {
    events.push_back(event);
  }

  std::vector<svp::builder::ProgressEvent> events;
};

bool has_event(const std::vector<svp::builder::ProgressEvent>& events,
               svp::builder::ProgressEventKind kind,
               svp::builder::ProgressStageId stage) {
  for (const auto& e : events) {
    if (e.kind == kind && e.stage_id == stage) return true;
  }
  return false;
}

void test_interlace_create_emits_progress_events() {
  const auto root = make_test_dir("svp_interlace_progress_create");
  const auto source_path = root / "source.mp4";
  create_mock_source_media(source_path, 512);
  const auto svpi_path = root / "output.svpi";

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source_path.string();
  opts.output_path = svpi_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.core_only_diagnostic = true;
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::media_binding));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::media_binding));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::svpi_write));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::svpi_write));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::validate));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::artifact_written,
                  svp::builder::ProgressStageId::svpi_write));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_emits_progress_events passed\n";
}

void test_interlace_validate_emits_progress_events() {
  const auto root = make_test_dir("svp_interlace_progress_validate");
  const auto source_path = root / "source.mp4";
  create_mock_source_media(source_path, 512);
  const auto svpi_path = root / "output.svpi";

  CHECK(build_minimal_svpi(svpi_path, source_path, true));

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::InterlaceValidateOptions opts;
  opts.svpi_path = svpi_path.string();
  opts.media_path = source_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_validate(opts);
  CHECK(result.structure_valid);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::validate));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::validate));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::media_binding));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_validate_emits_progress_events passed\n";
}

void test_interlace_extract_emits_progress_events() {
  const auto root = make_test_dir("svp_interlace_progress_extract");
  const auto source_path = root / "source.mp4";
  create_mock_source_media(source_path, 512);
  const auto svp_path = root / "test.svp";
  const auto out_dir = root / "extracted";

  CHECK(build_minimal_svp(svp_path, source_path));

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::InterlaceExtractOptions opts;
  opts.svp_path = svp_path.string();
  opts.out_dir = out_dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_extract(opts);
  CHECK(result.success);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::extract));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::extract));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::artifact_written,
                  svp::builder::ProgressStageId::extract));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_extract_emits_progress_events passed\n";
}

void test_interlace_recombine_emits_progress_events() {
  const auto root = make_test_dir("svp_interlace_progress_recombine");
  const auto source_path = root / "source.mp4";
  create_mock_source_media(source_path, 512);
  const auto svpi_path = root / "test.svpi";
  const auto svp_out = root / "recombined.svp";

  CHECK(build_minimal_svpi(svpi_path, source_path, true));

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::InterlaceRecombineOptions opts;
  opts.media_path = source_path.string();
  opts.svpi_path = svpi_path.string();
  opts.output_path = svp_out.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_recombine(opts);
  CHECK(result.success);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::validate));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::validate));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::media_binding));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::media_binding));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::recombine));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::recombine));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::artifact_written,
                  svp::builder::ProgressStageId::recombine));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_recombine_emits_progress_events passed\n";
}

void test_interlace_create_removes_default_staging() {
  auto root = make_test_dir("svp-staging-cleanup-ic-default");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "output.svpi";
  auto expected_staging = std::filesystem::path(svpi_path.string() + ".staging");

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source.string();
  opts.output_path = svpi_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svpi_path));
  CHECK(!std::filesystem::exists(expected_staging));

  auto parent = svpi_path.parent_path();
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    CHECK(entry.path().filename().string().find(".staging") == std::string::npos);
  }

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_removes_default_staging passed\n";
}

void test_interlace_create_core_only_no_visible_staging() {
  auto root = make_test_dir("svp-staging-cleanup-ic-core-only");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "output.svpi";
  auto expected_staging = std::filesystem::path(svpi_path.string() + ".staging");

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source.string();
  opts.output_path = svpi_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svpi_path));
  CHECK(!std::filesystem::exists(expected_staging));

  auto parent = svpi_path.parent_path();
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    CHECK(entry.path().filename().string().find(".staging") == std::string::npos);
  }

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_core_only_no_visible_staging passed\n";
}

void test_interlace_create_fallback_no_visible_staging() {
  auto root = make_test_dir("svp-staging-cleanup-ic-fallback");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "output.svpi";
  auto expected_staging = std::filesystem::path(svpi_path.string() + ".staging");

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source.string();
  opts.output_path = svpi_path.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.compute_full_blake3 = true;
  opts.compute_chunk_proof = false;
  opts.model_cache_dir = svp::builder::test::write_valid_model_cache(
                             root / "model-cache").string();

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svpi_path));
  CHECK(!std::filesystem::exists(expected_staging));

  auto parent = svpi_path.parent_path();
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    CHECK(entry.path().filename().string().find(".staging") == std::string::npos);
  }

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_fallback_no_visible_staging passed\n";
}

void test_interlace_create_preserves_explicit_staging() {
  auto root = make_test_dir("svp-staging-cleanup-ic-explicit");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "output.svpi";
  auto staging_dir = root / "my_staging";

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source.string();
  opts.output_path = svpi_path.string();
  opts.staging_dir = staging_dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svpi_path));
  CHECK(std::filesystem::exists(staging_dir));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_create_preserves_explicit_staging passed\n";
}

void test_interlace_recombine_removes_default_staging() {
  auto root = make_test_dir("svp-staging-cleanup-ir-default");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "test.svpi";
  auto svp_path = root / "recombined.svp";
  auto expected_staging = std::filesystem::path(svp_path.string() + ".staging");

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceRecombineOptions opts;
  opts.media_path = source.string();
  opts.svpi_path = svpi_path.string();
  opts.output_path = svp_path.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_recombine(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svp_path));
  CHECK(!std::filesystem::exists(expected_staging));

  auto parent = svp_path.parent_path();
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    CHECK(entry.path().filename().string().find(".staging") == std::string::npos);
  }

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_recombine_removes_default_staging passed\n";
}

void test_interlace_recombine_preserves_explicit_staging() {
  auto root = make_test_dir("svp-staging-cleanup-ir-explicit");
  auto source = root / "test.mov";
  create_mock_source_media(source, 512);
  auto svpi_path = root / "test.svpi";
  auto svp_path = root / "recombined.svp";
  auto staging_dir = root / "my_staging";

  CHECK(build_minimal_svpi(svpi_path, source, true));

  svp::builder::InterlaceRecombineOptions opts;
  opts.media_path = source.string();
  opts.svpi_path = svpi_path.string();
  opts.output_path = svp_path.string();
  opts.staging_dir = staging_dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_recombine(opts);
  CHECK(result.success);
  CHECK(std::filesystem::exists(svp_path));
  CHECK(std::filesystem::exists(staging_dir));

  std::filesystem::remove_all(root);
  std::cout << "  test_interlace_recombine_preserves_explicit_staging passed\n";
}

}  // namespace

int main() {
  std::cout << "Running SVPI Phase 2 interlace tests...\n";

  test_media_binding_factory_creates_binding_with_blake3();
  test_media_binding_verification_matching();
  test_media_binding_verification_wrong_file();
  test_interlace_create_produces_valid_svpi();
  test_interlace_create_falls_back_on_pipeline_failure();
  test_interlace_validate_structure_only();
  test_interlace_validate_with_matching_media();
  test_interlace_validate_with_wrong_media();
  test_interlace_inspect_shows_svpi_fields();
  test_interlace_extract_from_svp();
  test_interlace_recombine_produces_valid_svp();
  test_interlace_recombine_fails_on_wrong_media();
  test_filename_only_matching_does_not_pass_binding();
  test_missing_svpi_index_fails_validation();
  test_media_original_in_svpi_fails_validation();
  test_binding_verification_fails_on_unavailable_blake3();
  test_binding_verification_includes_container_and_streams();
  test_validator_rejects_binding_without_chunk_hashes();
  test_extract_preserves_existing_provenance_events();

  test_interlace_create_emits_progress_events();
  test_interlace_validate_emits_progress_events();
  test_interlace_extract_emits_progress_events();
  test_interlace_recombine_emits_progress_events();

  test_interlace_create_removes_default_staging();
  test_interlace_create_core_only_no_visible_staging();
  test_interlace_create_fallback_no_visible_staging();
  test_interlace_create_preserves_explicit_staging();
  test_interlace_recombine_removes_default_staging();
  test_interlace_recombine_preserves_explicit_staging();

  std::cout << "All SVPI Phase 2 interlace tests passed!\n";
  return 0;
}
