#include "svp/package/svpi_writer.hpp"
#include "svp/package/media_binding.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/package_summary.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/code_registry.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

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

svp::package::MediaBindingDocument make_minimal_media_binding(
    const std::filesystem::path& source_path) {
  svp::package::MediaBinding binding;
  binding.binding_id = "mb_primary_000001";
  binding.media_role = "primary_source";
  binding.media_id = "media_src_000001";
  binding.size_bytes = 0;
  binding.duration_us = 0;
  binding.container_format = "unknown";
  binding.verification_state = "pending";
  binding.identity.blake3_state = svp::package::Blake3State::pending;
  binding.identity.blake3_state_reason = "BLAKE3 not yet computed for Phase 1 proof";
  if (!source_path.empty()) {
    binding.location_hints.original_filename = source_path.filename().string();
  }

  svp::package::MediaBindingDocument doc;
  doc.primary_binding_id = "mb_primary_000001";
  doc.bindings.push_back(std::move(binding));

  return doc;
}

nlohmann::json make_svpi_manifest(const std::string& package_id) {
  return {
    {"format", "svpi"},
    {"svpi_version", "0.1"},
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

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

void write_text(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

void add_file_to_zip(zip_t* archive, const std::string& name,
                     const std::string& content) {
  zip_source_t* source =
      zip_source_buffer(archive, content.data(), content.size(), 0);
  CHECK(source != nullptr);
  zip_int64_t idx = zip_file_add(archive, name.c_str(), source, ZIP_FL_OVERWRITE);
  CHECK(idx >= 0);
}

std::filesystem::path create_valid_svpi(
    const std::filesystem::path& root,
    const std::string& package_id = "svpi_test_pkg_0001") {
  const std::filesystem::path staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);

  // Create provenance files
  write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
    {{"id", "processor_svpi_writer_0001"}, {"version", "0.1"}}
  });
  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    {{"event_id", "event_0001"},
     {"event_type", "svpi_created"},
     {"utc", make_utc_timestamp()}}
  });

  // Create index files
  const auto manifest = make_svpi_manifest(package_id);
  CHECK(svp::package::write_index_foundation(staging_dir, manifest));

  auto binding = make_minimal_media_binding("");

  const std::filesystem::path package_path = root / "output.svpi";
  CHECK(svp::package::write_svpi_package(package_path, staging_dir, manifest, binding));
  CHECK(std::filesystem::exists(package_path));

  return package_path;
}

svp::validation::SvpiValidatorOptions make_validator_options() {
  const auto codes_path = std::filesystem::path{SVP_SOURCE_DIR} / "spec" / "registries" /
                           "validation-codes.json";
  svp::validation::SvpiValidatorOptions opts;
  opts.validation_codes_path = codes_path;
  return opts;
}

void test_valid_minimal_svpi_passes_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-valid-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  auto layout_result = svp::package::read_package_layout(package_path);
  CHECK(layout_result.has_value());
  const auto& layout = layout_result.value();

  CHECK(layout.has_entry("mimetype"));
  CHECK(layout.has_entry("manifest.json"));
  CHECK(layout.has_entry("media_binding.json"));
  CHECK(layout.has_entry("provenance/processors.jsonl"));
  CHECK(layout.has_entry("provenance/interlace_events.jsonl"));
  CHECK(layout.has_entry("index/index.sqlite"));
  CHECK(layout.has_entry("index/index_manifest.json"));

  for (const auto& entry : layout.entries) {
    CHECK(entry.rfind("media/original/", 0) != 0);
  }

  auto mime = svp::package::read_package_entry(package_path, "mimetype");
  CHECK(mime.has_value());
  CHECK(mime.value() == "application/vnd.svp.interlace+zip");

  auto manifest_data = svp::package::read_package_entry(package_path, "manifest.json");
  CHECK(manifest_data.has_value());
  auto manifest = nlohmann::json::parse(manifest_data.value());
  CHECK(manifest["format"] == "svpi");
  CHECK(manifest["svpi_version"] == "0.1");

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::valid ||
        report.status == svp::validation::ValidationStatus::valid_with_warnings);
  CHECK(report.errors.empty());

  std::filesystem::remove_all(root);
}

void test_svpi_with_media_original_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-media-original-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  {
    int err = 0;
    zip_t* archive = zip_open(package_path.string().c_str(), 0, &err);
    CHECK(archive != nullptr);
    std::string content = "fake media bytes";
    add_file_to_zip(archive, "media/original/source_000.mov", content);
    CHECK(zip_close(archive) == 0);
  }

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_forbidden = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiForbiddenPrimaryMedia}) {
      found_forbidden = true;
    }
  }
  CHECK(found_forbidden);

  std::filesystem::remove_all(root);
}

void test_svpi_missing_index_sqlite_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-no-sqlite-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  // Re-create the zip without index/index.sqlite
  const std::filesystem::path temp_path = root / "temp.svpi";
  {
    int err = 0;
    zip_t* src = zip_open(package_path.string().c_str(), ZIP_RDONLY, &err);
    CHECK(src != nullptr);
    zip_t* dst = zip_open(temp_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    CHECK(dst != nullptr);

    const auto count = zip_get_num_entries(src, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
      zip_stat_t st;
      zip_stat_init(&st);
      zip_stat_index(src, static_cast<zip_uint64_t>(i), 0, &st);
      const std::string name(st.name);
      if (name == "index/index.sqlite") {
        continue;
      }
      zip_source_t* source = zip_source_zip(dst, src, static_cast<zip_uint64_t>(i), 0, 0, -1);
      CHECK(source != nullptr);
      zip_file_add(dst, name.c_str(), source, ZIP_FL_OVERWRITE);
    }

    CHECK(zip_close(dst) == 0);
    zip_discard(src);
  }
  std::filesystem::rename(temp_path, package_path);

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_missing_index = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiMissingIndex}) {
      found_missing_index = true;
    }
  }
  CHECK(found_missing_index);

  std::filesystem::remove_all(root);
}

void test_svpi_missing_index_manifest_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-no-index-manifest-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  // Re-create the zip without index/index_manifest.json
  const std::filesystem::path temp_path = root / "temp.svpi";
  {
    int err = 0;
    zip_t* src = zip_open(package_path.string().c_str(), ZIP_RDONLY, &err);
    CHECK(src != nullptr);
    zip_t* dst = zip_open(temp_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    CHECK(dst != nullptr);

    const auto count = zip_get_num_entries(src, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
      zip_stat_t st;
      zip_stat_init(&st);
      zip_stat_index(src, static_cast<zip_uint64_t>(i), 0, &st);
      const std::string name(st.name);
      if (name == "index/index_manifest.json") {
        continue;
      }
      zip_source_t* source = zip_source_zip(dst, src, static_cast<zip_uint64_t>(i), 0, 0, -1);
      CHECK(source != nullptr);
      zip_file_add(dst, name.c_str(), source, ZIP_FL_OVERWRITE);
    }

    CHECK(zip_close(dst) == 0);
    zip_discard(src);
  }
  std::filesystem::rename(temp_path, package_path);

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_missing_index = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiMissingIndex}) {
      found_missing_index = true;
    }
  }
  CHECK(found_missing_index);

  std::filesystem::remove_all(root);
}

void test_svpi_with_svpi_manifest_name_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-legacy-manifest-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  {
    int err = 0;
    zip_t* archive = zip_open(package_path.string().c_str(), 0, &err);
    CHECK(archive != nullptr);
    std::string content = "{\"format\": \"svpi\"}";
    add_file_to_zip(archive, "svpi_manifest.json", content);
    CHECK(zip_close(archive) == 0);
  }

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_legacy = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiLegacyManifestName}) {
      found_legacy = true;
    }
  }
  CHECK(found_legacy);

  std::filesystem::remove_all(root);
}

void test_svpi_wrong_manifest_format_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-wrong-format-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);
  write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
    {{"id", "processor_0001"}, {"version", "0.1"}}
  });
  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    {{"event_id", "event_0001"}, {"event_type", "svpi_created"},
     {"utc", make_utc_timestamp()}}
  });

  // Manifest with wrong format
  nlohmann::json manifest = make_svpi_manifest("svpi_test_wrong_format");
  manifest["format"] = "svp";  // Wrong format

  CHECK(svp::package::write_index_foundation(staging_dir, manifest));
  auto binding = make_minimal_media_binding("");

  const auto package_path = root / "output.svpi";
  CHECK(svp::package::write_svpi_package(package_path, staging_dir, manifest, binding));

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_wrong_format = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiWrongManifestFormat}) {
      found_wrong_format = true;
    }
  }
  CHECK(found_wrong_format);

  std::filesystem::remove_all(root);
}

void test_svpi_missing_manifest_format_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-missing-format-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);
  write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
    {{"id", "processor_0001"}, {"version", "0.1"}}
  });
  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    {{"event_id", "event_0001"}, {"event_type", "svpi_created"},
     {"utc", make_utc_timestamp()}}
  });

  // Manifest without format field
  nlohmann::json manifest = make_svpi_manifest("svpi_test_missing_format");
  manifest.erase("format");

  CHECK(svp::package::write_index_foundation(staging_dir, manifest));
  auto binding = make_minimal_media_binding("");

  const auto package_path = root / "output.svpi";
  CHECK(svp::package::write_svpi_package(package_path, staging_dir, manifest, binding));

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_wrong_format = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiWrongManifestFormat}) {
      found_wrong_format = true;
    }
  }
  CHECK(found_wrong_format);

  std::filesystem::remove_all(root);
}

void test_svpi_probe_detects_svpi_extension() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-probe-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  auto probe = svp::package::probe_package(package_path);
  CHECK(probe.exists);
  CHECK(probe.is_regular_file);
  CHECK(probe.has_svpi_extension);
  CHECK(!probe.has_svp_extension);

  std::filesystem::remove_all(root);
}

void test_svpi_inspector_recognizes_svpi() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-inspector-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  auto summary = svp::package::read_package_summary(package_path);
  CHECK(summary.probe.has_svpi_extension);
  CHECK(summary.svpi.is_svpi);
  CHECK(summary.svpi.svpi_version == "0.1");
  CHECK(summary.svpi.media_binding_ref == "media_binding.json");
  CHECK(summary.svpi.primary_media_binding_id == "mb_primary_000001");
  CHECK(!summary.svpi.has_media_original);
  CHECK(summary.svpi.media_binding.file.parsed);
  CHECK(summary.svpi.media_binding.binding_contract == "svpi.media_identity.v0.1");
  CHECK(summary.svpi.media_binding.verification_state == "pending");

  std::filesystem::remove_all(root);
}

void test_svpi_writer_excludes_media_original() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-exclude-media-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);

  // Create a file under media/original/ in staging
  write_text(staging_dir / "media" / "original" / "source_000.mov", "fake media");

  write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
    {{"id", "processor_0001"}, {"version", "0.1"}}
  });
  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    {{"event_id", "event_0001"}, {"event_type", "svpi_created"},
     {"utc", make_utc_timestamp()}}
  });

  const auto manifest = make_svpi_manifest("svpi_test_exclude_media");
  CHECK(svp::package::write_index_foundation(staging_dir, manifest));
  auto binding = make_minimal_media_binding("");

  const auto package_path = root / "output.svpi";
  CHECK(svp::package::write_svpi_package(package_path, staging_dir, manifest, binding));

  auto layout_result = svp::package::read_package_layout(package_path);
  CHECK(layout_result.has_value());
  for (const auto& entry : layout_result.value().entries) {
    CHECK(entry.rfind("media/original/", 0) != 0);
  }

  std::filesystem::remove_all(root);
}

void test_svpi_media_binding_json_structure() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-binding-json-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  auto binding_data = svp::package::read_package_entry(package_path, "media_binding.json");
  CHECK(binding_data.has_value());
  auto binding = nlohmann::json::parse(binding_data.value());

  CHECK(binding["schema"] == "svpi.media_binding.v0.1");
  CHECK(binding["primary_binding_id"] == "mb_primary_000001");
  CHECK(binding["bindings"].is_array());
  CHECK(binding["bindings"].size() == 1);
  const auto& b = binding["bindings"][0];
  CHECK(b["binding_id"] == "mb_primary_000001");
  CHECK(b["media_role"] == "primary_source");
  CHECK(b["media_id"] == "media_src_000001");
  CHECK(b["binding_contract"] == "svpi.media_identity.v0.1");
  CHECK(b["verification_state"] == "pending");
  CHECK(b["identity"]["full_file_blake3"]["state"] == "pending");
  CHECK(b["location_hints"].is_object());

  std::filesystem::remove_all(root);
}

void test_svpi_writer_fails_without_required_spine_files() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-missing-spine-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging_dir = root / "staging";
  std::filesystem::create_directories(staging_dir);

  // Only create provenance, skip index files
  write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
    {{"id", "processor_0001"}, {"version", "0.1"}}
  });
  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    {{"event_id", "event_0001"}, {"event_type", "svpi_created"},
     {"utc", make_utc_timestamp()}}
  });

  const auto manifest = make_svpi_manifest("svpi_test_missing_spine");
  auto binding = make_minimal_media_binding("");

  const auto package_path = root / "output.svpi";
  CHECK(!svp::package::write_svpi_package(package_path, staging_dir, manifest, binding));
  CHECK(!std::filesystem::exists(package_path));

  std::filesystem::remove_all(root);
}

void test_svpi_malformed_index_manifest_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-malformed-index-manifest-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  // Replace index/index_manifest.json with invalid JSON
  const std::filesystem::path temp_path = root / "temp.svpi";
  {
    int err = 0;
    zip_t* src = zip_open(package_path.string().c_str(), ZIP_RDONLY, &err);
    CHECK(src != nullptr);
    zip_t* dst = zip_open(temp_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    CHECK(dst != nullptr);

    const auto count = zip_get_num_entries(src, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
      zip_stat_t st;
      zip_stat_init(&st);
      zip_stat_index(src, static_cast<zip_uint64_t>(i), 0, &st);
      const std::string name(st.name);
      if (name == "index/index_manifest.json") {
        std::string bad_content = "this is not valid json {{{";
        zip_source_t* source =
            zip_source_buffer(dst, bad_content.data(), bad_content.size(), 0);
        CHECK(source != nullptr);
        zip_file_add(dst, "index/index_manifest.json", source, ZIP_FL_OVERWRITE);
        continue;
      }
      zip_source_t* source = zip_source_zip(dst, src, static_cast<zip_uint64_t>(i), 0, 0, -1);
      CHECK(source != nullptr);
      zip_file_add(dst, name.c_str(), source, ZIP_FL_OVERWRITE);
    }

    CHECK(zip_close(dst) == 0);
    zip_discard(src);
  }
  std::filesystem::rename(temp_path, package_path);

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_index_error = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiMissingIndex}) {
      found_index_error = true;
    }
  }
  CHECK(found_index_error);

  std::filesystem::remove_all(root);
}

void test_svpi_invalid_index_sqlite_fails_validation() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-svpi-invalid-sqlite-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto package_path = create_valid_svpi(root);

  // Replace index/index.sqlite with invalid content
  const std::filesystem::path temp_path = root / "temp.svpi";
  {
    int err = 0;
    zip_t* src = zip_open(package_path.string().c_str(), ZIP_RDONLY, &err);
    CHECK(src != nullptr);
    zip_t* dst = zip_open(temp_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    CHECK(dst != nullptr);

    const auto count = zip_get_num_entries(src, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
      zip_stat_t st;
      zip_stat_init(&st);
      zip_stat_index(src, static_cast<zip_uint64_t>(i), 0, &st);
      const std::string name(st.name);
      if (name == "index/index.sqlite") {
        std::string bad_content = "not a sqlite database";
        zip_source_t* source =
            zip_source_buffer(dst, bad_content.data(), bad_content.size(), 0);
        CHECK(source != nullptr);
        zip_file_add(dst, "index/index.sqlite", source, ZIP_FL_OVERWRITE);
        continue;
      }
      zip_source_t* source = zip_source_zip(dst, src, static_cast<zip_uint64_t>(i), 0, 0, -1);
      CHECK(source != nullptr);
      zip_file_add(dst, name.c_str(), source, ZIP_FL_OVERWRITE);
    }

    CHECK(zip_close(dst) == 0);
    zip_discard(src);
  }
  std::filesystem::rename(temp_path, package_path);

  auto opts = make_validator_options();
  auto report = svp::validation::validate_svpi_package(package_path, opts);
  CHECK(report.status == svp::validation::ValidationStatus::invalid);
  bool found_index_error = false;
  for (const auto& err : report.errors) {
    if (err.code == std::string{svp::validation::kCodeSvpiMissingIndex}) {
      found_index_error = true;
    }
  }
  CHECK(found_index_error);

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_svpi_probe_detects_svpi_extension();
  test_valid_minimal_svpi_passes_validation();
  test_svpi_with_media_original_fails_validation();
  test_svpi_missing_index_sqlite_fails_validation();
  test_svpi_missing_index_manifest_fails_validation();
  test_svpi_with_svpi_manifest_name_fails_validation();
  test_svpi_wrong_manifest_format_fails_validation();
  test_svpi_missing_manifest_format_fails_validation();
  test_svpi_inspector_recognizes_svpi();
  test_svpi_writer_excludes_media_original();
  test_svpi_media_binding_json_structure();
  test_svpi_writer_fails_without_required_spine_files();
  test_svpi_malformed_index_manifest_fails_validation();
  test_svpi_invalid_index_sqlite_fails_validation();

  std::cout << "All SVPI tests passed.\n";
  return 0;
}
