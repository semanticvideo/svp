#include "svp/builder/interlace_batch.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

#include <zip.h>

namespace svp::builder {

namespace {

constexpr std::string_view kSupportedVideoExts[] = {
    ".mov", ".mp4", ".mkv", ".avi", ".webm", ".m4v", ".wmv", ".flv"
};

bool is_supported_video(const std::filesystem::path& path) {
  const auto ext = path.extension().string();
  std::string ext_lower;
  ext_lower.reserve(ext.size());
  for (char c : ext) {
    ext_lower.push_back(static_cast<char>(std::tolower(c)));
  }
  for (auto supported : kSupportedVideoExts) {
    if (ext_lower == supported) return true;
  }
  return false;
}

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

nlohmann::json make_svpi_manifest(
    const std::string& source_filename,
    const svp::package::MediaBindingDocument& binding_doc) {
  nlohmann::json sections = nlohmann::json::object();
  sections["transcript"] = {{"state", "not_generated"}};
  sections["timeline"] = {{"state", "not_generated"}};
  sections["text"] = {{"state", "not_generated"}};
  sections["colors"] = {{"state", "not_generated"}};
  sections["entities"] = {{"state", "not_generated"}};
  sections["spatial"] = {{"state", "not_generated"}};
  sections["relationships"] = {{"state", "not_generated"}};
  sections["embeddings"] = {{"state", "not_generated"}};

  std::string package_id =
      "svpi_" + std::filesystem::path(source_filename).stem().string() + "_pkg";

  return {
    {"format", "svpi"},
    {"svpi_version", std::string{svp::package::kSvpiVersion}},
    {"svp_version", "1.0-rc.2"},
    {"package_id", package_id},
    {"created_utc", make_utc_timestamp()},
    {"media_binding_ref", "media_binding.json"},
    {"primary_media_binding_id", binding_doc.primary_binding_id},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }},
    {"sections", sections}
  };
}

bool create_single_svpi(
    const std::filesystem::path& source_path,
    const std::filesystem::path& svpi_output_path,
    const std::string& ffprobe_path,
    const std::string& ffmpeg_path,
    bool compute_full_blake3,
    const std::string& staging_dir_override,
    std::string& error_message,
    std::string& blake3_state_out) {

  svp::package::MediaBindingFactoryOptions binding_opts;
  binding_opts.ffprobe_path = ffprobe_path;
  binding_opts.compute_full_blake3 = compute_full_blake3;
  binding_opts.compute_chunk_proof = true;

  auto binding_doc = svp::package::create_media_binding(source_path, binding_opts);
  blake3_state_out = svp::package::to_string(
      binding_doc.bindings[0].identity.blake3_state);

  auto manifest = make_svpi_manifest(source_path.filename().string(), binding_doc);

  std::filesystem::path staging_dir;
  if (!staging_dir_override.empty()) {
    staging_dir = staging_dir_override;
  } else {
    staging_dir = svpi_output_path.string() + ".staging";
  }
  std::filesystem::remove_all(staging_dir);
  std::filesystem::create_directories(staging_dir);

  std::filesystem::create_directories(staging_dir / "provenance");
  std::filesystem::create_directories(staging_dir / "index");

  write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
    nlohmann::json{
      {"processor_id", "proc_interlace_create_000001"},
      {"processor_name", "svp-builder-interlace"},
      {"processor_version", "1.0.0"},
      {"stage", "interlace-create"},
      {"ran_utc", make_utc_timestamp()},
      {"inputs", nlohmann::json::array({source_path.filename().string()})},
      {"outputs", nlohmann::json::array({svpi_output_path.filename().string()})},
      {"notes", "SVPI sidecar created from source media without embedding primary media bytes"}
    }
  });

  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    nlohmann::json{
      {"event_id", "evt_interlace_create_000001"},
      {"event_type", "svpi_created_from_media"},
      {"event_utc", make_utc_timestamp()},
      {"authority", "builder_derived"},
      {"source_media", source_path.filename().string()},
      {"binding_id", binding_doc.primary_binding_id},
      {"binding_contract", std::string{svp::package::kSvpiBindingContract}},
      {"blake3_state", blake3_state_out}
    }
  });

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    error_message = "failed to write index foundation";
    return false;
  }

  bool success = svp::package::write_svpi_package(
      svpi_output_path, staging_dir, manifest, binding_doc);

  if (!success) {
    error_message = "failed to write SVPI package";
    return false;
  }

  return true;
}

bool check_svpi_valid_and_bound(
    const std::filesystem::path& svpi_path,
    const std::filesystem::path& media_path,
    const std::string& validation_codes_path,
    std::string& error_message) {

  svp::validation::SvpiValidatorOptions vopts;
  vopts.validation_codes_path = validation_codes_path;
  auto report = svp::validation::validate_svpi_package(svpi_path, vopts);
  if (svp::validation::exit_code(report) != 0) {
    error_message = "SVPI structure validation failed";
    for (const auto& err : report.errors) {
      error_message += "\n  " + err.code + ": " + err.message;
    }
    return false;
  }

  auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
  if (!binding_entry.has_value()) {
    error_message = "could not read media_binding.json";
    return false;
  }

  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  auto verification = svp::package::verify_media_binding(media_path, binding_doc);

  if (verification.state != svp::package::BindingVerificationState::verified) {
    error_message = "binding mismatch: " + verification.state_label;
    return false;
  }

  return true;
}

std::vector<std::filesystem::path> discover_media_files(
    const std::filesystem::path& dir, bool recursive) {
  std::vector<std::filesystem::path> result;
  if (recursive) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && is_supported_video(entry.path())) {
        result.push_back(entry.path());
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file() && is_supported_video(entry.path())) {
        result.push_back(entry.path());
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::filesystem::path> discover_svpi_files(
    const std::filesystem::path& dir, bool recursive) {
  std::vector<std::filesystem::path> result;
  auto collect = [&](const std::filesystem::path& path) {
    if (path.extension() == ".svpi") {
      result.push_back(path);
    }
  };
  if (recursive) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        collect(entry.path());
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        collect(entry.path());
      }
    }
    auto managed = dir / ".svpi";
    if (std::filesystem::exists(managed) && std::filesystem::is_directory(managed)) {
      for (const auto& entry : std::filesystem::directory_iterator(managed)) {
        if (entry.is_regular_file()) {
          collect(entry.path());
        }
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::filesystem::path find_candidate_sidecar(
    const std::filesystem::path& media_path,
    const std::filesystem::path& search_dir) {
  std::string stem = media_path.stem().string();
  std::vector<std::filesystem::path> candidates = {
    search_dir / (stem + ".svpi"),
    search_dir / ("." + stem + ".svpi"),
    search_dir / ".svpi" / (stem + ".svpi"),
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return {};
}

std::filesystem::path media_search_dir_for_svpi(
    const std::filesystem::path& svpi_path) {
  auto parent = svpi_path.parent_path();
  if (parent.filename() == ".svpi") {
    return parent.parent_path();
  }
  return parent;
}

std::string sidecar_stem(const std::filesystem::path& svpi_path) {
  std::string stem = svpi_path.stem().string();
  if (!stem.empty() && stem[0] == '.') {
    stem = stem.substr(1);
  }
  return stem;
}

}  // namespace

std::string_view sidecar_visibility_name(SidecarVisibility v) noexcept {
  switch (v) {
    case SidecarVisibility::visible: return "visible";
    case SidecarVisibility::hidden: return "hidden";
    case SidecarVisibility::managed_dir: return "managed-dir";
  }
  return "visible";
}

std::optional<SidecarVisibility> parse_sidecar_visibility(std::string_view v) noexcept {
  if (v == "visible") return SidecarVisibility::visible;
  if (v == "hidden") return SidecarVisibility::hidden;
  if (v == "managed-dir") return SidecarVisibility::managed_dir;
  return std::nullopt;
}

std::filesystem::path resolve_sidecar_path(
    const std::filesystem::path& media_path,
    SidecarVisibility visibility,
    const std::filesystem::path& out_dir) {
  std::string stem = media_path.stem().string();
  std::string filename;
  switch (visibility) {
    case SidecarVisibility::visible:
      filename = stem + ".svpi";
      break;
    case SidecarVisibility::hidden:
      filename = "." + stem + ".svpi";
      break;
    case SidecarVisibility::managed_dir:
      return out_dir / ".svpi" / (stem + ".svpi");
  }
  return out_dir / filename;
}

std::string_view batch_file_status_label(BatchFileStatus s) noexcept {
  switch (s) {
    case BatchFileStatus::created: return "created";
    case BatchFileStatus::already_valid: return "already_valid";
    case BatchFileStatus::skipped_unsupported: return "skipped_unsupported";
    case BatchFileStatus::binding_mismatch: return "binding_mismatch";
    case BatchFileStatus::failed: return "failed";
    case BatchFileStatus::replaced: return "replaced";
  }
  return "failed";
}

BatchCreateResult interlace_create_batch(const BatchCreateOptions& options) {
  BatchCreateResult result;

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    BatchFileResult r;
    r.status = BatchFileStatus::failed;
    r.error_message = "source directory does not exist: " + options.source_dir;
    result.results.push_back(std::move(r));
    result.failed_count = 1;
    return result;
  }

  const std::filesystem::path out_dir =
      options.out_dir.empty() || options.out_dir == "same-as-source"
          ? source_dir
          : std::filesystem::path(options.out_dir);

  if (!std::filesystem::exists(out_dir)) {
    std::filesystem::create_directories(out_dir);
  }

  if (options.visibility == SidecarVisibility::managed_dir) {
    std::filesystem::create_directories(out_dir / ".svpi");
  }

  auto media_files = discover_media_files(source_dir, options.recursive);

  for (const auto& media_path : media_files) {
    BatchFileResult file_result;
    file_result.source_filename = media_path.filename().string();
    file_result.source_relative_path =
        std::filesystem::relative(media_path, source_dir).string();

    const bool use_out_dir =
        !options.out_dir.empty() && options.out_dir != "same-as-source";
    const auto local_out_dir =
        use_out_dir ? out_dir : media_path.parent_path();

    file_result.svpi_path = resolve_sidecar_path(
        media_path, options.visibility, local_out_dir);

    if (std::filesystem::exists(file_result.svpi_path)) {
      std::string err;
      if (check_svpi_valid_and_bound(
              file_result.svpi_path, media_path,
              "spec/registries/validation-codes.json", err)) {
        file_result.status = BatchFileStatus::already_valid;
        result.already_valid_count++;
      } else {
        if (options.replace_mismatched) {
          std::string blake3_state;
          std::string create_err;
          if (create_single_svpi(
                  media_path, file_result.svpi_path,
                  options.ffprobe_path, options.ffmpeg_path,
                  !options.no_blake3, options.staging_dir,
                  create_err, blake3_state)) {
            file_result.status = BatchFileStatus::replaced;
            file_result.blake3_state = blake3_state;
            result.replaced_count++;
          } else {
            file_result.status = BatchFileStatus::failed;
            file_result.error_message = create_err;
            result.failed_count++;
          }
        } else {
          file_result.status = BatchFileStatus::binding_mismatch;
          file_result.error_message = err;
          result.mismatch_count++;
        }
      }
    } else {
      std::string blake3_state;
      std::string create_err;
      if (create_single_svpi(
              media_path, file_result.svpi_path,
              options.ffprobe_path, options.ffmpeg_path,
              !options.no_blake3, options.staging_dir,
              create_err, blake3_state)) {
        file_result.status = BatchFileStatus::created;
        file_result.blake3_state = blake3_state;
        result.created_count++;
      } else {
        file_result.status = BatchFileStatus::failed;
        file_result.error_message = create_err;
        result.failed_count++;
      }
    }

    result.results.push_back(std::move(file_result));
  }

  return result;
}

std::string_view batch_validation_state_label(BatchValidationState s) noexcept {
  switch (s) {
    case BatchValidationState::valid_bound: return "valid_bound";
    case BatchValidationState::valid_unbound: return "valid_unbound";
    case BatchValidationState::binding_mismatch: return "binding_mismatch";
    case BatchValidationState::invalid_structure: return "invalid_structure";
    case BatchValidationState::failed: return "failed";
  }
  return "failed";
}

ScanResult interlace_scan(const ScanOptions& options) {
  ScanResult result;

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    return result;
  }

  auto media_files = discover_media_files(source_dir, options.recursive);
  auto svpi_files = discover_svpi_files(source_dir, options.recursive);

  result.total_media = static_cast<int>(media_files.size());
  result.total_svpi = static_cast<int>(svpi_files.size());

  std::set<std::filesystem::path> matched_svpi;

  for (const auto& media_path : media_files) {
    ScanPairInfo pair;
    pair.media_path = media_path;
    pair.media_filename = media_path.filename().string();
    pair.media_relative_path =
        std::filesystem::relative(media_path, source_dir).string();

    auto candidate = find_candidate_sidecar(media_path, media_path.parent_path());
    if (!candidate.empty()) {
      pair.svpi_path = candidate;
      pair.svpi_found = true;
      matched_svpi.insert(candidate);

      svp::validation::SvpiValidatorOptions vopts;
      vopts.validation_codes_path = options.validation_codes_path;
      auto report = svp::validation::validate_svpi_package(candidate, vopts);

      if (svp::validation::exit_code(report) == 0) {
        auto binding_entry = svp::package::read_package_entry(candidate, "media_binding.json");
        if (binding_entry.has_value()) {
          auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
          auto verification = svp::package::verify_media_binding(media_path, binding_doc);
          pair.binding_state_label = verification.state_label;
          pair.binding_verified =
              (verification.state == svp::package::BindingVerificationState::verified);
          if (pair.binding_verified) {
            result.verified_pairs++;
          }
        }
      } else {
        pair.svpi_error = "structure validation failed";
      }

      result.matched_pairs++;
    } else {
      result.missing_sidecars.push_back(pair.media_relative_path);
    }

    result.pairs.push_back(std::move(pair));
  }

  for (const auto& svpi_path : svpi_files) {
    if (matched_svpi.find(svpi_path) == matched_svpi.end()) {
      auto rel = std::filesystem::relative(svpi_path, source_dir).string();
      result.unbound_sidecars.push_back(rel);
    }
  }

  return result;
}

BatchValidateResult interlace_validate_batch(const BatchValidateOptions& options) {
  BatchValidateResult result;

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    return result;
  }

  auto svpi_files = discover_svpi_files(source_dir, options.recursive);

  for (const auto& svpi_path : svpi_files) {
    BatchValidateFileResult file_result;
    file_result.svpi_path = svpi_path;
    file_result.svpi_filename = svpi_path.filename().string();
    file_result.svpi_relative_path =
        std::filesystem::relative(svpi_path, source_dir).string();

    svp::validation::SvpiValidatorOptions vopts;
    vopts.validation_codes_path = options.validation_codes_path;
    auto report = svp::validation::validate_svpi_package(svpi_path, vopts);

    if (svp::validation::exit_code(report) != 0) {
      file_result.state = BatchValidationState::invalid_structure;
      for (const auto& err : report.errors) {
        file_result.errors.push_back(err.code + ": " + err.message);
      }
      result.invalid_structure_count++;
      result.results.push_back(std::move(file_result));
      continue;
    }

    auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
    if (!binding_entry.has_value()) {
      file_result.state = BatchValidationState::invalid_structure;
      file_result.errors.push_back("could not read media_binding.json");
      result.invalid_structure_count++;
      result.results.push_back(std::move(file_result));
      continue;
    }

    auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());

    std::filesystem::path media_dir = media_search_dir_for_svpi(svpi_path);
    std::string stem = sidecar_stem(svpi_path);

    std::vector<std::filesystem::path> media_candidates;
    for (auto ext : kSupportedVideoExts) {
      media_candidates.push_back(media_dir / (stem + std::string(ext)));
    }

    std::filesystem::path found_media;
    for (const auto& candidate : media_candidates) {
      if (std::filesystem::exists(candidate)) {
        found_media = candidate;
        break;
      }
    }

    if (found_media.empty()) {
      file_result.state = BatchValidationState::valid_unbound;
      result.valid_unbound_count++;
    } else {
      file_result.media_filename = found_media.filename().string();
      auto verification = svp::package::verify_media_binding(found_media, binding_doc);
      if (verification.state == svp::package::BindingVerificationState::verified) {
        file_result.state = BatchValidationState::valid_bound;
        result.valid_bound_count++;
      } else {
        file_result.state = BatchValidationState::binding_mismatch;
        for (const auto& check : verification.failing_checks) {
          file_result.errors.push_back(check);
        }
        result.mismatch_count++;
      }
    }

    result.results.push_back(std::move(file_result));
  }

  return result;
}

CompleteIdentityResult interlace_complete_identity(const CompleteIdentityOptions& options) {
  CompleteIdentityResult result;

  const std::filesystem::path svpi_path(options.svpi_path);
  const std::filesystem::path media_path(options.media_path);

  if (!std::filesystem::exists(svpi_path)) {
    result.error_message = "SVPI file does not exist: " + options.svpi_path;
    return result;
  }
  if (!std::filesystem::exists(media_path)) {
    result.error_message = "media file does not exist: " + options.media_path;
    return result;
  }

  svp::validation::SvpiValidatorOptions vopts;
  vopts.validation_codes_path = options.validation_codes_path;
  auto report = svp::validation::validate_svpi_package(svpi_path, vopts);
  if (svp::validation::exit_code(report) != 0) {
    result.error_message = "SVPI structure validation failed";
    return result;
  }

  auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
  if (!binding_entry.has_value()) {
    result.error_message = "could not read media_binding.json";
    return result;
  }

  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  if (binding_doc.bindings.empty()) {
    result.error_message = "no bindings in media_binding.json";
    return result;
  }

  result.previous_state = svp::package::to_string(
      binding_doc.bindings[0].identity.blake3_state);

  auto verification = svp::package::verify_media_binding(media_path, binding_doc);

  if (binding_doc.bindings[0].identity.blake3_state == svp::package::Blake3State::pending) {
    if (verification.state == svp::package::BindingVerificationState::pending) {
      bool size_ok = false;
      for (const auto& check : verification.passing_checks) {
        if (check == "size_bytes") {
          size_ok = true;
          break;
        }
      }
      if (!size_ok) {
        result.error_message =
            "media binding verification failed: size_bytes mismatch";
        return result;
      }
      svp::package::MediaBindingFactoryOptions probe_opts;
      probe_opts.ffprobe_path = options.ffprobe_path;
      probe_opts.compute_full_blake3 = false;
      probe_opts.compute_chunk_proof = true;
      probe_opts.chunk_size_bytes =
          binding_doc.bindings[0].identity.chunk_proof.has_value()
              ? binding_doc.bindings[0].identity.chunk_proof->chunk_size_bytes
              : 16777216;
      auto candidate_doc = svp::package::create_media_binding(media_path, probe_opts);
      const auto& cand = candidate_doc.bindings[0];
      const auto& existing = binding_doc.bindings[0];

      if (cand.duration_us > 0 && existing.duration_us > 0 &&
          cand.duration_us != existing.duration_us) {
        result.error_message = "media binding verification failed: duration_us mismatch";
        return result;
      }
      if (!cand.container_format.empty() && cand.container_format != "unknown" &&
          !existing.container_format.empty() && existing.container_format != "unknown" &&
          cand.container_format != existing.container_format) {
        result.error_message = "media binding verification failed: container_format mismatch";
        return result;
      }
      if (existing.identity.chunk_proof.has_value() &&
          cand.identity.chunk_proof.has_value()) {
        const auto& expected = *existing.identity.chunk_proof;
        const auto& actual = *cand.identity.chunk_proof;
        if (actual.chunk_count != expected.chunk_count) {
          result.error_message = "media binding verification failed: chunk_proof chunk_count mismatch";
          return result;
        }
        if (actual.last_chunk_hash != expected.last_chunk_hash) {
          result.error_message = "media binding verification failed: chunk_proof last_chunk_hash mismatch";
          return result;
        }
        if (actual.last_chunk_size != expected.last_chunk_size) {
          result.error_message = "media binding verification failed: chunk_proof last_chunk_size mismatch";
          return result;
        }
      }
    } else if (verification.state != svp::package::BindingVerificationState::verified) {
      result.error_message =
          "media binding verification failed: " + verification.state_label;
      return result;
    }
  } else {
    if (verification.state != svp::package::BindingVerificationState::verified) {
      result.error_message =
          "media binding verification failed: " + verification.state_label;
      return result;
    }
  }

  if (binding_doc.bindings[0].identity.blake3_state == svp::package::Blake3State::present) {
    result.new_state = result.previous_state;
    result.blake3_hash = binding_doc.bindings[0].identity.blake3_hash;
    result.success = true;
    return result;
  }

  svp::package::MediaBindingFactoryOptions factory_opts;
  factory_opts.ffprobe_path = options.ffprobe_path;
  factory_opts.compute_full_blake3 = true;
  factory_opts.compute_chunk_proof = true;
  factory_opts.binding_id = binding_doc.bindings[0].binding_id;
  factory_opts.media_id = binding_doc.bindings[0].media_id;

  auto new_binding_doc = svp::package::create_media_binding(media_path, factory_opts);

  binding_doc.bindings[0].identity = new_binding_doc.bindings[0].identity;
  binding_doc.bindings[0].verification_state = "verified";

  result.new_state = svp::package::to_string(
      binding_doc.bindings[0].identity.blake3_state);
  result.blake3_hash = binding_doc.bindings[0].identity.blake3_hash;

  auto manifest_entry = svp::package::read_package_entry(svpi_path, "manifest.json");
  if (!manifest_entry.has_value()) {
    result.error_message = "could not read manifest.json";
    return result;
  }
  auto manifest = nlohmann::json::parse(manifest_entry.value(), nullptr, false);
  if (manifest.is_discarded() || !manifest.is_object()) {
    result.error_message = "manifest.json is not valid JSON";
    return result;
  }

  std::filesystem::path staging_dir = svpi_path.string() + ".identity-staging";
  std::filesystem::remove_all(staging_dir);
  std::filesystem::create_directories(staging_dir);

  std::filesystem::create_directories(staging_dir / "provenance");
  std::filesystem::create_directories(staging_dir / "index");

  int zip_error = ZIP_ER_OK;
  zip_t* src_archive = zip_open(svpi_path.string().c_str(), ZIP_RDONLY, &zip_error);
  if (!src_archive) {
    result.error_message = "could not open SVPI as ZIP";
    return result;
  }

  const std::vector<std::string> sections_to_copy = {
    "transcript/", "timeline/", "entities/", "spatial/",
    "text/", "colors/", "relationships/", "embeddings/",
    "index/", "provenance/"
  };

  const auto num_entries = zip_get_num_entries(src_archive, 0);
  for (zip_int64_t i = 0; i < num_entries; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(src_archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0) continue;
    if (stat.name == nullptr) continue;
    std::string name(stat.name);
    if (name.back() == '/') continue;

    bool skip = true;
    for (const auto& section : sections_to_copy) {
      if (name.rfind(section, 0) == 0) {
        skip = false;
        break;
      }
    }
    if (skip) continue;

    zip_file_t* file = zip_fopen(src_archive, name.c_str(), 0);
    if (!file) continue;

    auto dest = staging_dir / name;
    std::filesystem::create_directories(dest.parent_path());
    std::ofstream out(dest, std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    zip_int64_t count;
    while ((count = zip_fread(file, buffer.data(), buffer.size())) > 0) {
      out.write(buffer.data(), static_cast<std::streamsize>(count));
    }
    zip_fclose(file);
  }
  zip_close(src_archive);

  {
    auto events_path = staging_dir / "provenance" / "interlace_events.jsonl";
    std::vector<nlohmann::json> existing_events;
    if (std::filesystem::exists(events_path)) {
      std::ifstream in(events_path);
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty()) {
          auto j = nlohmann::json::parse(line, nullptr, false);
          if (!j.is_discarded()) {
            existing_events.push_back(std::move(j));
          }
        }
      }
    }
    existing_events.push_back(nlohmann::json{
      {"event_id", "evt_complete_identity_000001"},
      {"event_type", "svpi_identity_completed"},
      {"event_utc", make_utc_timestamp()},
      {"authority", "builder_derived"},
      {"source_svpi", svpi_path.filename().string()},
      {"source_media", media_path.filename().string()},
      {"binding_id", binding_doc.primary_binding_id},
      {"previous_blake3_state", result.previous_state},
      {"new_blake3_state", result.new_state}
    });
    write_jsonl(events_path, existing_events);
  }

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    result.error_message = "failed to rebuild index foundation";
    return result;
  }

  bool success = svp::package::write_svpi_package(
      svpi_path, staging_dir, manifest, binding_doc);

  if (!success) {
    result.error_message = "failed to rewrite SVPI package";
    return result;
  }

  result.success = true;
  result.rebuilt = true;
  return result;
}

CompleteIdentityBatchResult interlace_complete_identity_batch(
    const CompleteIdentityBatchOptions& options) {
  CompleteIdentityBatchResult result;

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    return result;
  }

  auto svpi_files = discover_svpi_files(source_dir, options.recursive);

  for (const auto& svpi_path : svpi_files) {
    result.svpi_filenames.push_back(svpi_path.filename().string());

    std::string stem = sidecar_stem(svpi_path);
    std::filesystem::path media_dir = media_search_dir_for_svpi(svpi_path);
    std::filesystem::path found_media;
    for (auto ext : kSupportedVideoExts) {
      auto candidate = media_dir / (stem + std::string(ext));
      if (std::filesystem::exists(candidate)) {
        found_media = candidate;
        break;
      }
    }

    if (found_media.empty()) {
      CompleteIdentityResult r;
      r.error_message = "no candidate media found for " + svpi_path.filename().string();
      result.results.push_back(std::move(r));
      result.failed_count++;
      continue;
    }

    CompleteIdentityOptions opts;
    opts.svpi_path = svpi_path.string();
    opts.media_path = found_media.string();
    opts.ffprobe_path = options.ffprobe_path;
    opts.validation_codes_path = options.validation_codes_path;

    auto r = interlace_complete_identity(opts);
    if (r.success) {
      if (r.rebuilt) {
        result.completed_count++;
      } else {
        result.already_present_count++;
      }
    } else {
      result.failed_count++;
    }
    result.results.push_back(std::move(r));
  }

  return result;
}

}  // namespace svp::builder
