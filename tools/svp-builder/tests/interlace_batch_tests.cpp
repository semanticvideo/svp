#include "svp/builder/interlace.hpp"
#include "svp/builder/interlace_batch.hpp"
#include "svp/builder/build_progress.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/package_summary.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/validator.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/query/query_ops.hpp"

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
    opts.ffprobe_path = "/usr/bin/true";
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

}  // namespace

// --- Phase 3 tests ---

void test_visible_sidecar_name_resolution() {
  auto media = std::filesystem::path("/tmp/test/video.mov");
  auto svpi = svp::builder::resolve_sidecar_path(
      media, svp::builder::SidecarVisibility::visible, media.parent_path());
  CHECK(svpi.filename().string() == "video.svpi");
  std::cout << "  test_visible_sidecar_name_resolution passed\n";
}

void test_hidden_sidecar_name_resolution() {
  auto media = std::filesystem::path("/tmp/test/video.mov");
  auto svpi = svp::builder::resolve_sidecar_path(
      media, svp::builder::SidecarVisibility::hidden, media.parent_path());
  CHECK(svpi.filename().string() == ".video.svpi");
  std::cout << "  test_hidden_sidecar_name_resolution passed\n";
}

void test_managed_dir_sidecar_name_resolution() {
  auto media = std::filesystem::path("/tmp/test/video.mov");
  auto svpi = svp::builder::resolve_sidecar_path(
      media, svp::builder::SidecarVisibility::managed_dir, media.parent_path());
  CHECK(svpi.parent_path().filename().string() == ".svpi");
  CHECK(svpi.filename().string() == "video.svpi");
  std::cout << "  test_managed_dir_sidecar_name_resolution passed\n";
}

void test_batch_create_creates_sidecars() {
  auto root = make_test_dir("svp-phase3-batch-create");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip1.mov";
  auto media2 = dir / "clip2.mp4";
  create_mock_source_media(media1, 512);
  create_mock_source_media(media2, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.visibility = svp::builder::SidecarVisibility::visible;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 2);
  CHECK(result.failed_count == 0);
  CHECK(std::filesystem::exists(dir / "clip1.svpi"));
  CHECK(std::filesystem::exists(dir / "clip2.svpi"));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_creates_sidecars passed\n";
}

void test_no_duplicate_on_rerun() {
  auto root = make_test_dir("svp-phase3-no-duplicate");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.core_only_diagnostic = true;

  auto result1 = svp::builder::interlace_create_batch(opts);
  CHECK(result1.created_count == 1);

  auto result2 = svp::builder::interlace_create_batch(opts);
  CHECK(result2.created_count == 0);
  CHECK(result2.already_valid_count == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_no_duplicate_on_rerun passed\n";
}

void test_existing_mismatched_not_overwritten() {
  auto root = make_test_dir("svp-phase3-mismatch-no-overwrite");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip.mov";
  auto media2 = dir / "other.mov";
  create_mock_source_media(media1, 512);
  create_mock_source_media(media2, 256);

  CHECK(build_minimal_svpi(dir / "clip.svpi", media1, true));

  auto media1_modified = dir / "clip.mov";
  create_mock_source_media(media1_modified, 999);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.replace_mismatched = false;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  bool found_mismatch = false;
  for (const auto& r : result.results) {
    if (r.status == svp::builder::BatchFileStatus::binding_mismatch) {
      found_mismatch = true;
    }
  }
  CHECK(found_mismatch);
  CHECK(result.created_count == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_existing_mismatched_not_overwritten passed\n";
}

void test_batch_create_hidden_sidecars() {
  auto root = make_test_dir("svp-phase3-hidden-create");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.visibility = svp::builder::SidecarVisibility::hidden;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 1);
  CHECK(std::filesystem::exists(dir / ".clip.svpi"));
  CHECK(!std::filesystem::exists(dir / "clip.svpi"));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_hidden_sidecars passed\n";
}

void test_batch_create_managed_dir_sidecars() {
  auto root = make_test_dir("svp-phase3-managed-dir-create");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.visibility = svp::builder::SidecarVisibility::managed_dir;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 1);
  CHECK(std::filesystem::exists(dir / ".svpi" / "clip.svpi"));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_managed_dir_sidecars passed\n";
}

void test_scan_detects_missing_sidecar() {
  auto root = make_test_dir("svp-phase3-scan-missing");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::ScanOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_scan(opts);
  CHECK(result.total_media == 1);
  CHECK(result.total_svpi == 0);
  CHECK(result.missing_sidecars.size() == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_scan_detects_missing_sidecar passed\n";
}

void test_scan_detects_unbound_sidecar() {
  auto root = make_test_dir("svp-phase3-scan-unbound");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto svpi = dir / "orphan.svpi";
  auto fake_media = root / "fake.mov";
  create_mock_source_media(fake_media, 256);
  CHECK(build_minimal_svpi(svpi, fake_media, true));

  svp::builder::ScanOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_scan(opts);
  CHECK(result.total_svpi == 1);
  CHECK(result.unbound_sidecars.size() == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_scan_detects_unbound_sidecar passed\n";
}

void test_scan_finds_verified_pair() {
  auto root = make_test_dir("svp-phase3-scan-verified");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media, true));

  svp::builder::ScanOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_scan(opts);
  CHECK(result.total_media == 1);
  CHECK(result.matched_pairs == 1);
  CHECK(result.verified_pairs == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_scan_finds_verified_pair passed\n";
}

void test_validate_batch_reports_valid_bound_and_unbound() {
  auto root = make_test_dir("svp-phase3-validate-batch");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media, true));

  auto dir2 = root / "unbound";
  std::filesystem::create_directories(dir2);
  auto fake_media = root / "fake.mov";
  create_mock_source_media(fake_media, 256);
  CHECK(build_minimal_svpi(dir2 / "unbound.svpi", fake_media, true));

  svp::builder::BatchValidateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_validate_batch(opts);
  CHECK(result.valid_bound_count == 1);

  svp::builder::BatchValidateOptions opts2;
  opts2.source_dir = dir2.string();
  opts2.ffprobe_path = "/usr/bin/true";

  auto result2 = svp::builder::interlace_validate_batch(opts2);
  CHECK(result2.valid_unbound_count == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_validate_batch_reports_valid_bound_and_unbound passed\n";
}

void test_validate_batch_reports_mismatch() {
  auto root = make_test_dir("svp-phase3-validate-mismatch");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip.mov";
  create_mock_source_media(media1, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media1, true));

  create_mock_source_media(media1, 999);

  svp::builder::BatchValidateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_validate_batch(opts);
  CHECK(result.mismatch_count == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_validate_batch_reports_mismatch passed\n";
}

void test_wrong_renamed_media_does_not_verify() {
  auto root = make_test_dir("svp-phase3-wrong-media");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip.mov";
  create_mock_source_media(media1, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media1, true));

  auto media2 = dir / "different.mov";
  create_mock_source_media(media2, 256);

  svp::builder::InterlaceValidateOptions vopts;
  vopts.svpi_path = (dir / "clip.svpi").string();
  vopts.media_path = media2.string();
  vopts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_validate(vopts);
  CHECK(result.structure_valid);
  CHECK(result.binding_attempted);
  CHECK(!result.binding_verified);

  std::filesystem::remove_all(root);
  std::cout << "  test_wrong_renamed_media_does_not_verify passed\n";
}

void test_complete_identity_updates_pending_blake3() {
  auto root = make_test_dir("svp-phase3-complete-pending");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = dir.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.no_blake3 = true;

  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 1);

  svp::builder::CompleteIdentityOptions opts;
  opts.svpi_path = (dir / "clip.svpi").string();
  opts.media_path = media.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_complete_identity(opts);
  CHECK(result.success);
  CHECK(result.previous_state == "pending");
  CHECK(result.new_state == "present");
  CHECK(result.rebuilt);

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_updates_pending_blake3 passed\n";
}

void test_complete_identity_refuses_mismatch() {
  auto root = make_test_dir("svp-phase3-complete-mismatch");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip.mov";
  create_mock_source_media(media1, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = dir.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.no_blake3 = true;
  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 1);

  auto media2 = dir / "wrong.mov";
  create_mock_source_media(media2, 256);

  svp::builder::CompleteIdentityOptions opts;
  opts.svpi_path = (dir / "clip.svpi").string();
  opts.media_path = media2.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_complete_identity(opts);
  CHECK(!result.success);
  CHECK(!result.error_message.empty());

  svp::validation::SvpiValidatorOptions vopts;
  vopts.validation_codes_path = "spec/registries/validation-codes.json";
  auto report = svp::validation::validate_svpi_package(dir / "clip.svpi", vopts);
  CHECK(svp::validation::exit_code(report) == 0);

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_refuses_mismatch passed\n";
}

void test_complete_identity_already_present() {
  auto root = make_test_dir("svp-phase3-complete-already-present");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media, true));

  svp::builder::CompleteIdentityOptions opts;
  opts.svpi_path = (dir / "clip.svpi").string();
  opts.media_path = media.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_complete_identity(opts);
  CHECK(result.success);
  CHECK(result.previous_state == "present");
  CHECK(result.new_state == "present");
  CHECK(!result.rebuilt);

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_already_present passed\n";
}

void test_single_svpi_layer_query() {
  auto root = make_test_dir("svp-phase3-svpi-query");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media, true));

  auto layers = svp::query::list_layers(dir / "clip.svpi");
  CHECK(layers.total_entries > 0);

  bool found_index = false;
  for (const auto& layer : layers.layers) {
    if (layer.section == "index") {
      found_index = true;
    }
  }
  CHECK(found_index);

  std::filesystem::remove_all(root);
  std::cout << "  test_single_svpi_layer_query passed\n";
}

void test_svpi_inspect_summary() {
  auto root = make_test_dir("svp-phase3-svpi-inspect");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media, true));

  auto summary = svp::package::read_package_summary(dir / "clip.svpi");
  CHECK(summary.svpi.is_svpi);
  CHECK(summary.probe.has_svpi_extension);

  std::filesystem::remove_all(root);
  std::cout << "  test_svpi_inspect_summary passed\n";
}

void test_recombination_still_works() {
  auto root = make_test_dir("svp-phase3-recombination");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);
  CHECK(build_minimal_svpi(dir / "clip.svpi", media, true));

  svp::builder::InterlaceRecombineOptions opts;
  opts.media_path = media.string();
  opts.svpi_path = (dir / "clip.svpi").string();
  opts.output_path = (dir / "recombined.svp").string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_recombine(opts);
  CHECK(result.success);
  CHECK(result.binding_verified);
  CHECK(std::filesystem::exists(dir / "recombined.svp"));

  auto layout = svp::package::read_package_layout(dir / "recombined.svp");
  CHECK(layout.has_value());
  CHECK(layout.value().has_entry("manifest.json"));
  bool has_media = false;
  for (const auto& entry : layout.value().entries) {
    if (entry.rfind("media/original/", 0) == 0 && entry.back() != '/') {
      has_media = true;
      break;
    }
  }
  CHECK(has_media);

  std::filesystem::remove_all(root);
  std::cout << "  test_recombination_still_works passed\n";
}

void test_batch_create_recursive() {
  auto root = make_test_dir("svp-phase3-recursive");
  auto dir = root / "videos";
  auto subdir = dir / "subfolder";
  std::filesystem::create_directories(subdir);

  auto media1 = dir / "clip1.mov";
  auto media2 = subdir / "clip2.mp4";
  create_mock_source_media(media1, 512);
  create_mock_source_media(media2, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.recursive = true;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 2);
  CHECK(std::filesystem::exists(dir / "clip1.svpi"));
  CHECK(std::filesystem::exists(subdir / "clip2.svpi"));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_recursive passed\n";
}

void test_batch_create_skips_unsupported() {
  auto root = make_test_dir("svp-phase3-skip-unsupported");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  auto text_file = dir / "readme.txt";
  create_mock_source_media(media, 512);
  create_mock_source_media(text_file, 100);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 1);
  CHECK(!std::filesystem::exists(dir / "readme.svpi"));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_skips_unsupported passed\n";
}

void test_scan_discovers_hidden_sidecar() {
  auto root = make_test_dir("svp-phase3-scan-hidden");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.visibility = svp::builder::SidecarVisibility::hidden;
  opts.core_only_diagnostic = true;

  auto create_result = svp::builder::interlace_create_batch(opts);
  CHECK(create_result.created_count == 1);

  svp::builder::ScanOptions scan_opts;
  scan_opts.source_dir = dir.string();
  scan_opts.ffprobe_path = "/usr/bin/true";

  auto scan_result = svp::builder::interlace_scan(scan_opts);
  CHECK(scan_result.total_media == 1);
  CHECK(scan_result.matched_pairs == 1);
  CHECK(scan_result.verified_pairs == 1);

  std::filesystem::remove_all(root);
  std::cout << "  test_scan_discovers_hidden_sidecar passed\n";
}

void test_complete_identity_batch() {
  auto root = make_test_dir("svp-phase3-complete-batch");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip1.mov";
  auto media2 = dir / "clip2.mov";
  create_mock_source_media(media1, 512);
  create_mock_source_media(media2, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = dir.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.no_blake3 = true;
  create_opts.core_only_diagnostic = true;
  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 2);

  svp::builder::CompleteIdentityBatchOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_complete_identity_batch(opts);
  CHECK(result.completed_count == 2);
  CHECK(result.failed_count == 0);

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_batch passed\n";
}

void test_complete_identity_rejects_same_size_wrong_content() {
  auto root = make_test_dir("svp-phase3-complete-same-size-wrong");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media1 = dir / "clip.mov";
  create_mock_source_media(media1, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = dir.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.no_blake3 = true;
  create_opts.core_only_diagnostic = true;
  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 1);

  auto media2 = dir / "wrong.mov";
  std::ofstream out(media2, std::ios::binary);
  for (int i = 0; i < 512; ++i) {
    out.put(static_cast<char>((i * 7 + 13) % 256));
  }
  out.close();

  svp::builder::CompleteIdentityOptions opts;
  opts.svpi_path = (dir / "clip.svpi").string();
  opts.media_path = media2.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_complete_identity(opts);
  CHECK(!result.success);
  CHECK(!result.error_message.empty());

  svp::validation::SvpiValidatorOptions vopts;
  vopts.validation_codes_path = "spec/registries/validation-codes.json";
  auto report = svp::validation::validate_svpi_package(dir / "clip.svpi", vopts);
  CHECK(svp::validation::exit_code(report) == 0);

  auto binding_entry = svp::package::read_package_entry(dir / "clip.svpi", "media_binding.json");
  CHECK(binding_entry.has_value());
  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  CHECK(binding_doc.bindings[0].identity.blake3_state == svp::package::Blake3State::pending);

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_rejects_same_size_wrong_content passed\n";
}

void test_validate_batch_managed_dir() {
  auto root = make_test_dir("svp-phase3-validate-managed-dir");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = dir.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.visibility = svp::builder::SidecarVisibility::managed_dir;
  create_opts.core_only_diagnostic = true;
  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 1);
  CHECK(std::filesystem::exists(dir / ".svpi" / "clip.svpi"));

  svp::builder::BatchValidateOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_validate_batch(opts);
  CHECK(result.valid_bound_count == 1);
  CHECK(result.valid_unbound_count == 0);
  CHECK(result.mismatch_count == 0);

  std::filesystem::remove_all(root);
  std::cout << "  test_validate_batch_managed_dir passed\n";
}

void test_complete_identity_batch_managed_dir() {
  auto root = make_test_dir("svp-phase3-complete-batch-managed-dir");
  auto dir = root / "videos";
  std::filesystem::create_directories(dir);

  auto media = dir / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = dir.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.visibility = svp::builder::SidecarVisibility::managed_dir;
  create_opts.no_blake3 = true;
  create_opts.core_only_diagnostic = true;
  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 1);

  svp::builder::CompleteIdentityBatchOptions opts;
  opts.source_dir = dir.string();
  opts.ffprobe_path = "/usr/bin/true";

  auto result = svp::builder::interlace_complete_identity_batch(opts);
  CHECK(result.completed_count == 1);
  CHECK(result.failed_count == 0);

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_batch_managed_dir passed\n";
}

void test_batch_create_out_dir_visible() {
  auto root = make_test_dir("svp-phase3-out-dir-visible");
  auto src = root / "src";
  auto out = root / "out";
  std::filesystem::create_directories(src);

  auto media = src / "clip.mov";
  create_mock_source_media(media, 512);

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = src.string();
  opts.out_dir = out.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.visibility = svp::builder::SidecarVisibility::visible;
  opts.core_only_diagnostic = true;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 1);
  CHECK(std::filesystem::exists(out / "clip.svpi"));
  CHECK(!std::filesystem::exists(src / "clip.svpi"));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_out_dir_visible passed\n";
}

// --- Progress event tests ---

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

int count_events(const std::vector<svp::builder::ProgressEvent>& events,
                 svp::builder::ProgressEventKind kind,
                 svp::builder::ProgressStageId stage) {
  int count = 0;
  for (const auto& e : events) {
    if (e.kind == kind && e.stage_id == stage) ++count;
  }
  return count;
}

void test_batch_create_emits_progress_events() {
  const auto root = make_test_dir("svp_batch_progress_create");
  const auto src = root / "src";
  std::filesystem::create_directories(src);
  const auto media1 = src / "clip1.mp4";
  const auto media2 = src / "clip2.mp4";
  create_mock_source_media(media1, 256);
  create_mock_source_media(media2, 256);

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::BatchCreateOptions opts;
  opts.source_dir = src.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.visibility = svp::builder::SidecarVisibility::visible;
  opts.core_only_diagnostic = true;
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_create_batch(opts);
  CHECK(result.created_count == 2);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::batch_scan));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::batch_scan));

  CHECK(count_events(sink->events, svp::builder::ProgressEventKind::stage_started,
                     svp::builder::ProgressStageId::batch_item) == 2);
  CHECK(count_events(sink->events, svp::builder::ProgressEventKind::stage_completed,
                     svp::builder::ProgressStageId::batch_item) == 2);

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_create_emits_progress_events passed\n";
}

void test_batch_validate_emits_progress_events() {
  const auto root = make_test_dir("svp_batch_progress_validate");
  const auto src = root / "src";
  std::filesystem::create_directories(src);
  const auto media1 = src / "clip1.mp4";
  create_mock_source_media(media1, 256);
  const auto svpi1 = src / "clip1.svpi";
  CHECK(build_minimal_svpi(svpi1, media1, true));

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::BatchValidateOptions opts;
  opts.source_dir = src.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_validate_batch(opts);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::batch_scan));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::batch_scan));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::batch_item));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::batch_item));

  std::filesystem::remove_all(root);
  std::cout << "  test_batch_validate_emits_progress_events passed\n";
}

void test_complete_identity_emits_progress_events() {
  const auto root = make_test_dir("svp_batch_progress_identity");
  const auto src = root / "src";
  std::filesystem::create_directories(src);
  const auto media1 = src / "clip1.mov";
  create_mock_source_media(media1, 512);

  svp::builder::BatchCreateOptions create_opts;
  create_opts.source_dir = src.string();
  create_opts.ffprobe_path = "/usr/bin/true";
  create_opts.no_blake3 = true;

  auto create_result = svp::builder::interlace_create_batch(create_opts);
  CHECK(create_result.created_count == 1);

  auto sink = std::make_shared<CapturingProgressSink>();

  svp::builder::CompleteIdentityOptions opts;
  opts.svpi_path = (src / "clip1.svpi").string();
  opts.media_path = media1.string();
  opts.ffprobe_path = "/usr/bin/true";
  opts.progress_sink = sink;

  auto result = svp::builder::interlace_complete_identity(opts);
  CHECK(result.success);
  CHECK(!sink->events.empty());

  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_started,
                  svp::builder::ProgressStageId::identity));
  CHECK(has_event(sink->events, svp::builder::ProgressEventKind::stage_completed,
                  svp::builder::ProgressStageId::identity));

  std::filesystem::remove_all(root);
  std::cout << "  test_complete_identity_emits_progress_events passed\n";
}

int main() {
  std::cout << "Running SVPI Phase 3 batch/scale tests...\n";

  test_visible_sidecar_name_resolution();
  test_hidden_sidecar_name_resolution();
  test_managed_dir_sidecar_name_resolution();
  test_batch_create_creates_sidecars();
  test_no_duplicate_on_rerun();
  test_existing_mismatched_not_overwritten();
  test_batch_create_hidden_sidecars();
  test_batch_create_managed_dir_sidecars();
  test_scan_detects_missing_sidecar();
  test_scan_detects_unbound_sidecar();
  test_scan_finds_verified_pair();
  test_validate_batch_reports_valid_bound_and_unbound();
  test_validate_batch_reports_mismatch();
  test_wrong_renamed_media_does_not_verify();
  test_complete_identity_updates_pending_blake3();
  test_complete_identity_refuses_mismatch();
  test_complete_identity_already_present();
  test_single_svpi_layer_query();
  test_svpi_inspect_summary();
  test_recombination_still_works();
  test_batch_create_recursive();
  test_batch_create_skips_unsupported();
  test_scan_discovers_hidden_sidecar();
  test_complete_identity_batch();
  test_complete_identity_rejects_same_size_wrong_content();
  test_validate_batch_managed_dir();
  test_complete_identity_batch_managed_dir();
  test_batch_create_out_dir_visible();

  test_batch_create_emits_progress_events();
  test_batch_validate_emits_progress_events();
  test_complete_identity_emits_progress_events();

  std::cout << "All SVPI Phase 3 batch/scale tests passed!\n";
  return 0;
}
