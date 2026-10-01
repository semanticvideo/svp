#include "svpi_publication.hpp"

#include "svp/package/index_writer.hpp"
#include "svp/package/output_directory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/validation/svpi_validator.hpp"

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
  svp::package::ensure_parent_directory(path);
  std::ofstream out(path);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

bool dir_has_files(const std::filesystem::path& dir) {
  if (!std::filesystem::exists(dir)) return false;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file()) return true;
  }
  return false;
}

nlohmann::json make_svpi_manifest_with_sections(
    const std::string& source_filename,
    const svp::package::MediaBindingDocument& binding_doc,
    const nlohmann::json& sections) {
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

nlohmann::json make_core_only_sections(const std::string& state) {
  nlohmann::json sections = nlohmann::json::object();
  for (const auto& key : {"transcript", "timeline", "text", "colors",
                          "entities", "spatial", "relationships", "embeddings"}) {
    sections[key] = {{"state", state}};
  }
  return sections;
}

void append_jsonl_if_exists(const std::filesystem::path& path,
                            const std::vector<nlohmann::json>& records) {
  svp::package::ensure_parent_directory(path);
  std::ofstream out(path, std::ios::app);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

}  // namespace

nlohmann::json detect_section_states(const std::filesystem::path& staging_dir) {
  auto state_for = [&](const std::string& subdir) -> std::string {
    return dir_has_files(staging_dir / subdir) ? "generated" : "not_generated";
  };
  nlohmann::json sections = nlohmann::json::object();
  sections["transcript"] = {{"state", state_for("transcript")}};
  sections["timeline"] = {{"state", state_for("timeline")}};
  sections["text"] = {{"state", state_for("text")}};
  sections["colors"] = {{"state", state_for("colors")}};
  sections["entities"] = {{"state", state_for("entities")}};
  sections["spatial"] = {{"state", state_for("spatial")}};
  sections["relationships"] = {{"state", state_for("relationships")}};
  sections["embeddings"] = {{"state", state_for("embeddings")}};
  return sections;
}

InterlaceCreateResult write_svpi_from_staging(
    const SvpiWriteRequest& options,
    const svp::package::MediaBindingDocument& binding_doc,
    const std::string& blake3_state,
    const std::filesystem::path& staging_dir,
    const nlohmann::json& sections,
    const std::string& provenance_notes,
    BuildProgressSink& sink) {
  InterlaceCreateResult result;
  result.svpi_path = options.output_path;
  result.blake3_state = blake3_state;

  sink.emit(make_stage_started(ProgressStageId::svpi_write));

  const std::filesystem::path source_path(options.source_path);

  append_jsonl_if_exists(staging_dir / "provenance" / "processors.jsonl", {
    nlohmann::json{
      {"processor_id", "proc_interlace_create_000001"},
      {"processor_name", "svp-builder-interlace"},
      {"processor_version", "1.0.0"},
      {"stage", "interlace-create"},
      {"ran_utc", make_utc_timestamp()},
      {"inputs", nlohmann::json::array({source_path.filename().string()})},
      {"outputs", nlohmann::json::array({std::filesystem::path(options.output_path).filename().string()})},
      {"notes", provenance_notes}
    }
  });

  append_jsonl_if_exists(staging_dir / "provenance" / "interlace_events.jsonl", {
    nlohmann::json{
      {"event_id", "evt_interlace_create_000001"},
      {"event_type", "svpi_created_from_media"},
      {"event_utc", make_utc_timestamp()},
      {"authority", "builder_derived"},
      {"source_media", source_path.filename().string()},
      {"binding_id", binding_doc.primary_binding_id},
      {"binding_contract", std::string{svp::package::kSvpiBindingContract}},
      {"blake3_state", blake3_state}
    }
  });

  auto manifest = make_svpi_manifest_with_sections(
      source_path.filename().string(), binding_doc, sections);

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    result.error_message = "failed to write index foundation";
    return result;
  }

  result.success = svp::package::write_svpi_package(
      options.output_path, staging_dir, manifest, binding_doc);

  if (!result.success) {
    result.error_message = "failed to write SVPI package";
    sink.emit(make_stage_failed(ProgressStageId::svpi_write, result.error_message));
    return result;
  }

  sink.emit(make_artifact_written(ProgressStageId::svpi_write, options.output_path));
  sink.emit(make_stage_completed(ProgressStageId::svpi_write));

  sink.emit(make_stage_started(ProgressStageId::validate));
  svp::validation::SvpiValidatorOptions validator_opts;
  auto report = svp::validation::validate_svpi_package(options.output_path, validator_opts);
  result.binding_state = svp::validation::to_string(report.status);

  if (svp::validation::exit_code(report) == 0) {
    sink.emit(make_stage_completed(ProgressStageId::validate));
  } else {
    sink.emit(make_stage_failed(ProgressStageId::validate, "SVPI validation reported issues"));
  }

  return result;
}

InterlaceCreateResult write_core_only_svpi(
    const SvpiWriteRequest& options,
    const svp::package::MediaBindingDocument& binding_doc,
    const std::string& blake3_state,
    const std::string& section_state,
    const std::string& notes,
    const std::filesystem::path& staging_dir,
    BuildProgressSink& sink) {
  InterlaceCreateResult result;
  result.svpi_path = options.output_path;
  result.blake3_state = blake3_state;

  sink.emit(make_stage_started(ProgressStageId::svpi_write));

  const std::filesystem::path source_path(options.source_path);

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
      {"notes", notes}
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
      {"blake3_state", blake3_state}
    }
  });

  auto sections = make_core_only_sections(section_state);
  auto manifest = make_svpi_manifest_with_sections(
      source_path.filename().string(), binding_doc, sections);

  auto manifest_copy = manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    result.error_message = "failed to write index foundation";
    return result;
  }

  result.success = svp::package::write_svpi_package(
      options.output_path, staging_dir, manifest, binding_doc);

  if (!result.success) {
    result.error_message = "failed to write SVPI package";
    sink.emit(make_stage_failed(ProgressStageId::svpi_write, result.error_message));
    return result;
  }

  sink.emit(make_artifact_written(ProgressStageId::svpi_write, options.output_path));
  sink.emit(make_stage_completed(ProgressStageId::svpi_write));

  sink.emit(make_stage_started(ProgressStageId::validate));
  svp::validation::SvpiValidatorOptions validator_opts;
  auto report = svp::validation::validate_svpi_package(options.output_path, validator_opts);
  result.binding_state = svp::validation::to_string(report.status);

  if (svp::validation::exit_code(report) == 0) {
    sink.emit(make_stage_completed(ProgressStageId::validate));
  } else {
    sink.emit(make_stage_failed(ProgressStageId::validate, "SVPI validation reported issues"));
  }

  return result;
}

bool has_semantic_content(const nlohmann::json& sections) {
  for (const auto& key : {"transcript", "timeline", "text", "colors",
                          "entities", "spatial", "relationships", "embeddings"}) {
    if (sections[key]["state"] == "generated") {
      return true;
    }
  }
  return false;
}

std::string svpi_provenance_notes(bool semantic_content) {
  return semantic_content
             ? "SVPI sidecar created from source media with semantic pipeline output, without embedding primary media bytes"
             : "SVPI sidecar created with core-only content (semantic pipeline produced no section output, sections marked not_generated)";
}

}  // namespace svp::builder
