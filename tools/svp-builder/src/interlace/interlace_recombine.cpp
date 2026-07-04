#include "svp/builder/interlace.hpp"
#include "svp/builder/build_progress.hpp"

#include "staging_cleanup.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/validator.hpp"
#include "svp/validation/svpi_validator.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace svp::builder {

namespace {

struct ZipDeleter {
  void operator()(zip_t* archive) const noexcept {
    if (archive != nullptr) {
      zip_discard(archive);
    }
  }
};

using ZipArchive = std::unique_ptr<zip_t, ZipDeleter>;

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

bool extract_zip_entry(zip_t* archive, const std::string& entry_name,
                       const std::filesystem::path& output_path) {
  zip_file_t* file = zip_fopen(archive, entry_name.c_str(), 0);
  if (!file) {
    return false;
  }

  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream out(output_path, std::ios::binary);
  if (!out) {
    zip_fclose(file);
    return false;
  }

  std::array<char, 64 * 1024> buffer{};
  zip_int64_t count;
  while ((count = zip_fread(file, buffer.data(), buffer.size())) > 0) {
    out.write(buffer.data(), static_cast<std::streamsize>(count));
  }

  zip_fclose(file);
  return out.good();
}

}  // namespace

InterlaceRecombineResult interlace_recombine(
    const InterlaceRecombineOptions& options) {
  InterlaceRecombineResult result;
  result.svp_path = options.output_path;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

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

  sink->emit(make_stage_started(ProgressStageId::validate, "SVPI structure"));
  svp::validation::SvpiValidatorOptions svpi_validator_opts;
  svpi_validator_opts.validation_codes_path = options.validation_codes_path;
  auto svpi_report = svp::validation::validate_svpi_package(svpi_path, svpi_validator_opts);
  if (svp::validation::exit_code(svpi_report) != 0) {
    result.error_message = "SVPI structure validation failed";
    for (const auto& err : svpi_report.errors) {
      result.error_message += "\n  " + err.code + ": " + err.message;
    }
    sink->emit(make_stage_failed(ProgressStageId::validate, "SVPI structure validation failed"));
    return result;
  }
  sink->emit(make_stage_completed(ProgressStageId::validate, "SVPI structure"));

  sink->emit(make_stage_started(ProgressStageId::media_binding, "binding verification"));
  auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
  if (!binding_entry.has_value()) {
    result.error_message = "could not read media_binding.json from SVPI";
    return result;
  }

  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  auto verification = svp::package::verify_media_binding(media_path, binding_doc);

  result.binding_state_label = verification.state_label;
  result.binding_passing_checks = verification.passing_checks;
  result.binding_failing_checks = verification.failing_checks;
  result.binding_verified =
      (verification.state == svp::package::BindingVerificationState::verified);

  if (!result.binding_verified) {
    result.error_message =
        "media binding verification failed: " + result.binding_state_label;
    for (const auto& check : result.binding_failing_checks) {
      result.error_message += "\n  " + check;
    }
    sink->emit(make_stage_failed(ProgressStageId::media_binding, result.binding_state_label));
    return result;
  }
  sink->emit(make_stage_completed(ProgressStageId::media_binding, "binding verified"));

  sink->emit(make_stage_started(ProgressStageId::recombine));

  auto manifest_entry = svp::package::read_package_entry(svpi_path, "manifest.json");
  if (!manifest_entry.has_value()) {
    result.error_message = "could not read manifest.json from SVPI";
    return result;
  }

  auto svpi_manifest = nlohmann::json::parse(manifest_entry.value(), nullptr, false);
  if (svpi_manifest.is_discarded() || !svpi_manifest.is_object()) {
    result.error_message = "manifest.json is not valid JSON";
    return result;
  }

  nlohmann::json svp_manifest = svpi_manifest;
  svp_manifest.erase("format");
  svp_manifest.erase("svpi_version");
  svp_manifest.erase("media_binding_ref");
  svp_manifest.erase("primary_media_binding_id");
  svp_manifest.erase("extraction_source");
  svp_manifest["svp_version"] = svpi_manifest.value("svp_version", "1.0-rc.2");
  svp_manifest["primary_media_id"] =
      binding_doc.bindings[0].media_id;
  if (!svp_manifest.contains("binary_block_format")) {
    svp_manifest["binary_block_format"] = {
      {"magic", "SVPB"},
      {"version", 1},
      {"header_size", 160},
      {"compression", "zstd"}
    };
  }
  if (!svp_manifest.contains("hashes")) {
    svp_manifest["hashes"] = {
      {"algorithm", "blake3"},
      {"digest_bytes", 32},
      {"encoding", "lowercase_hex"},
      {"manifest_excluded", true}
    };
  }
  if (!svp_manifest.contains("required_sections")) {
    svp_manifest["required_sections"] = {
      {"media", true}, {"transcript", true}, {"timeline", true},
      {"entities", true}, {"spatial", true}, {"relationships", true},
      {"embeddings", true}, {"index", true}, {"provenance", true}
    };
  }

  const bool user_supplied_staging = !options.staging_dir.empty();
  std::filesystem::path staging_dir;
  if (user_supplied_staging) {
    staging_dir = options.staging_dir;
  } else {
    staging_dir = std::filesystem::path(options.output_path + ".staging");
  }
  StagingCleanupGuard staging_guard(staging_dir, user_supplied_staging);
  std::filesystem::remove_all(staging_dir);
  std::filesystem::create_directories(staging_dir);

  int zip_error = ZIP_ER_OK;
  ZipArchive archive{zip_open(svpi_path.string().c_str(), ZIP_RDONLY, &zip_error)};
  if (!archive) {
    result.error_message = "could not open SVPI as ZIP archive";
    return result;
  }

  const std::vector<std::string> sections_to_copy = {
    "transcript/", "timeline/", "entities/", "spatial/",
    "text/", "colors/", "relationships/", "embeddings/",
    "index/", "provenance/"
  };

  const auto num_entries = zip_get_num_entries(archive.get(), 0);
  for (zip_int64_t i = 0; i < num_entries; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(i), 0, &stat) != 0) {
      continue;
    }
    if (stat.name == nullptr) {
      continue;
    }
    std::string name(stat.name);

    if (name.back() == '/') {
      continue;
    }

    bool skip = true;
    for (const auto& section : sections_to_copy) {
      if (name.rfind(section, 0) == 0) {
        skip = false;
        break;
      }
    }
    if (skip) {
      continue;
    }

    auto rel_path = std::filesystem::path(name);
    auto dest = staging_dir / rel_path;
    extract_zip_entry(archive.get(), name, dest);
  }

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
      {"event_id", "evt_interlace_recombine_000001"},
      {"event_type", "svp_recombined_from_svpi"},
      {"event_utc", make_utc_timestamp()},
      {"authority", "builder_derived"},
      {"source_svpi", svpi_path.filename().string()},
      {"source_media", media_path.filename().string()},
      {"binding_id", binding_doc.primary_binding_id},
      {"binding_verification", "verified"},
      {"output_svp", std::filesystem::path(options.output_path).filename().string()}
    });
    write_jsonl(events_path, existing_events);
  }

  auto manifest_copy = svp_manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    result.error_message = "failed to write index foundation for recombined SVP";
    return result;
  }

  result.success = svp::package::write_package_skeleton(
      options.output_path, staging_dir, media_path, svp_manifest);

  if (!result.success) {
    result.error_message = "failed to write recombined SVP package";
    sink->emit(make_stage_failed(ProgressStageId::recombine, result.error_message));
    return result;
  }

  sink->emit(make_artifact_written(ProgressStageId::recombine, options.output_path));
  sink->emit(make_stage_completed(ProgressStageId::recombine));

  sink->emit(make_stage_started(ProgressStageId::validate, "SVP validation"));
  svp::validation::ValidatorOptions validator_opts;
  validator_opts.validation_codes_path = options.validation_codes_path;
  result.validation_report =
      svp::validation::validate_package(options.output_path, validator_opts);

  if (svp::validation::exit_code(result.validation_report) == 0) {
    sink->emit(make_stage_completed(ProgressStageId::validate, "SVP validation"));
  } else {
    sink->emit(make_stage_failed(ProgressStageId::validate, "SVP validation reported issues"));
  }

  staging_guard.cleanup_on_success();
  return result;
}

}  // namespace svp::builder
