#include "svp/builder/interlace.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace svp::builder {

namespace {

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

}  // namespace

InterlaceCreateResult interlace_create(const InterlaceCreateOptions& options) {
  InterlaceCreateResult result;
  result.svpi_path = options.output_path;

  const std::filesystem::path source_path(options.source_path);
  if (!std::filesystem::exists(source_path)) {
    result.error_message = "source media file does not exist: " + options.source_path;
    return result;
  }

  svp::package::MediaBindingFactoryOptions binding_opts;
  binding_opts.ffprobe_path = options.ffprobe_path;
  binding_opts.compute_full_blake3 = options.compute_full_blake3;
  binding_opts.compute_chunk_proof = options.compute_chunk_proof;

  auto binding_doc = svp::package::create_media_binding(source_path, binding_opts);
  result.blake3_state = svp::package::to_string(binding_doc.bindings[0].identity.blake3_state);

  auto manifest = make_svpi_manifest(source_path.filename().string(), binding_doc);

  std::filesystem::path staging_dir;
  if (!options.staging_dir.empty()) {
    staging_dir = options.staging_dir;
  } else {
    staging_dir = std::filesystem::path(options.output_path + ".staging");
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
      {"outputs", nlohmann::json::array({std::filesystem::path(options.output_path).filename().string()})},
      {"notes", "SVPI sidecar created from source media without embedding primary media bytes"}
    }
  });

  write_jsonl(staging_dir / "provenance" / "interlace_events.jsonl", {
    nlohmann::json{
      {"event_id", "evt_interlace_create_000001"},
      {"event_type", "interlace_create"},
      {"timestamp_utc", make_utc_timestamp()},
      {"source_media", source_path.filename().string()},
      {"binding_id", binding_doc.primary_binding_id},
      {"binding_contract", std::string{svp::package::kSvpiBindingContract}},
      {"blake3_state", result.blake3_state}
    }
  });

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    result.error_message = "failed to write index foundation";
    return result;
  }

  result.success = svp::package::write_svpi_package(
      options.output_path, staging_dir, manifest, binding_doc);

  if (!result.success) {
    result.error_message = "failed to write SVPI package";
    return result;
  }

  svp::validation::SvpiValidatorOptions validator_opts;
  validator_opts.validation_codes_path = "spec/registries/validation-codes.json";
  auto report = svp::validation::validate_svpi_package(options.output_path, validator_opts);
  result.binding_state = svp::validation::to_string(report.status);

  return result;
}

}  // namespace svp::builder
