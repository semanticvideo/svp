#include "svp/builder/interlace_batch.hpp"
#include "svp/builder/build_progress.hpp"
#include "interlace_batch_internal.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <zip.h>

namespace svp::builder {

namespace {

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

}  // namespace

CompleteIdentityResult interlace_complete_identity(const CompleteIdentityOptions& options) {
  CompleteIdentityResult result;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  sink->emit(make_stage_started(ProgressStageId::identity,
      std::filesystem::path(options.svpi_path).filename().string()));

  const std::filesystem::path svpi_path(options.svpi_path);
  const std::filesystem::path media_path(options.media_path);

  if (!std::filesystem::exists(svpi_path)) {
    result.error_message = "SVPI file does not exist: " + options.svpi_path;
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }
  if (!std::filesystem::exists(media_path)) {
    result.error_message = "media file does not exist: " + options.media_path;
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }

  svp::validation::SvpiValidatorOptions vopts;
  vopts.validation_codes_path = options.validation_codes_path;
  auto report = svp::validation::validate_svpi_package(svpi_path, vopts);
  if (svp::validation::exit_code(report) != 0) {
    result.error_message = "SVPI structure validation failed";
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }

  auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
  if (!binding_entry.has_value()) {
    result.error_message = "could not read media_binding.json";
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }

  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  if (binding_doc.bindings.empty()) {
    result.error_message = "no bindings in media_binding.json";
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
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
        sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
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
        sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
        return result;
      }
      if (!cand.container_format.empty() && cand.container_format != "unknown" &&
          !existing.container_format.empty() && existing.container_format != "unknown" &&
          cand.container_format != existing.container_format) {
        result.error_message = "media binding verification failed: container_format mismatch";
        sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
        return result;
      }
      if (existing.identity.chunk_proof.has_value() &&
          cand.identity.chunk_proof.has_value()) {
        const auto& expected = *existing.identity.chunk_proof;
        const auto& actual = *cand.identity.chunk_proof;
        if (actual.chunk_count != expected.chunk_count) {
          result.error_message = "media binding verification failed: chunk_proof chunk_count mismatch";
          sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
          return result;
        }
        if (actual.last_chunk_hash != expected.last_chunk_hash) {
          result.error_message = "media binding verification failed: chunk_proof last_chunk_hash mismatch";
          sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
          return result;
        }
        if (actual.last_chunk_size != expected.last_chunk_size) {
          result.error_message = "media binding verification failed: chunk_proof last_chunk_size mismatch";
          sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
          return result;
        }
      }
    } else if (verification.state != svp::package::BindingVerificationState::verified) {
      result.error_message =
          "media binding verification failed: " + verification.state_label;
      sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
      return result;
    }
  } else {
    if (verification.state != svp::package::BindingVerificationState::verified) {
      result.error_message =
          "media binding verification failed: " + verification.state_label;
      sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
      return result;
    }
  }

  if (binding_doc.bindings[0].identity.blake3_state == svp::package::Blake3State::present) {
    result.new_state = result.previous_state;
    result.blake3_hash = binding_doc.bindings[0].identity.blake3_hash;
    result.success = true;
    sink->emit(make_stage_completed(ProgressStageId::identity, "already present"));
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
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }
  auto manifest = nlohmann::json::parse(manifest_entry.value(), nullptr, false);
  if (manifest.is_discarded() || !manifest.is_object()) {
    result.error_message = "manifest.json is not valid JSON";
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
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
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
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
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }

  bool success = svp::package::write_svpi_package(
      svpi_path, staging_dir, manifest, binding_doc);

  if (!success) {
    result.error_message = "failed to rewrite SVPI package";
    sink->emit(make_stage_failed(ProgressStageId::identity, result.error_message));
    return result;
  }

  result.success = true;
  result.rebuilt = true;
  sink->emit(make_artifact_written(ProgressStageId::identity, svpi_path.string()));
  sink->emit(make_stage_completed(ProgressStageId::identity, "blake3 completed"));
  return result;
}

CompleteIdentityBatchResult interlace_complete_identity_batch(
    const CompleteIdentityBatchOptions& options) {
  CompleteIdentityBatchResult result;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    return result;
  }

  sink->emit(make_stage_started(ProgressStageId::batch_scan, options.source_dir));
  auto svpi_files = discover_svpi_files(source_dir, options.recursive);
  sink->emit(make_stage_completed(ProgressStageId::batch_scan,
      std::to_string(svpi_files.size()) + " SVPI files found"));

  for (const auto& svpi_path : svpi_files) {
    sink->emit(make_stage_started(ProgressStageId::batch_item,
        svpi_path.filename().string()));

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
      sink->emit(make_stage_failed(ProgressStageId::batch_item,
          svpi_path.filename().string() + ": no candidate media found"));
      continue;
    }

    CompleteIdentityOptions opts;
    opts.svpi_path = svpi_path.string();
    opts.media_path = found_media.string();
    opts.ffprobe_path = options.ffprobe_path;
    opts.validation_codes_path = options.validation_codes_path;
    opts.progress_sink = sink;

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
    sink->emit(make_stage_completed(ProgressStageId::batch_item,
        svpi_path.filename().string()));
  }

  return result;
}

}  // namespace svp::builder
