#include "svp/builder/interlace.hpp"
#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"

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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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

bool dir_has_files(const std::filesystem::path& dir) {
  if (!std::filesystem::exists(dir)) return false;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file()) return true;
  }
  return false;
}

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
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::app);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

InterlaceCreateResult write_svpi_from_staging(
    const InterlaceCreateOptions& options,
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
  validator_opts.validation_codes_path = "spec/registries/validation-codes.json";
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
    const InterlaceCreateOptions& options,
    const svp::package::MediaBindingDocument& binding_doc,
    const std::string& blake3_state,
    const std::string& section_state,
    const std::string& notes,
    BuildProgressSink& sink) {
  InterlaceCreateResult result;
  result.svpi_path = options.output_path;
  result.blake3_state = blake3_state;

  sink.emit(make_stage_started(ProgressStageId::svpi_write));

  const std::filesystem::path source_path(options.source_path);

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
  validator_opts.validation_codes_path = "spec/registries/validation-codes.json";
  auto report = svp::validation::validate_svpi_package(options.output_path, validator_opts);
  result.binding_state = svp::validation::to_string(report.status);

  if (svp::validation::exit_code(report) == 0) {
    sink.emit(make_stage_completed(ProgressStageId::validate));
  } else {
    sink.emit(make_stage_failed(ProgressStageId::validate, "SVPI validation reported issues"));
  }

  return result;
}

}  // namespace

InterlaceCreateResult interlace_create(const InterlaceCreateOptions& options) {
  InterlaceCreateResult result;
  result.svpi_path = options.output_path;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  const std::filesystem::path source_path(options.source_path);
  if (!std::filesystem::exists(source_path)) {
    result.error_message = "source media file does not exist: " + options.source_path;
    return result;
  }

  sink->emit(make_stage_started(ProgressStageId::media_binding));
  svp::package::MediaBindingFactoryOptions binding_opts;
  binding_opts.ffprobe_path = options.ffprobe_path;
  binding_opts.compute_full_blake3 = options.compute_full_blake3;
  binding_opts.compute_chunk_proof = options.compute_chunk_proof;

  auto binding_doc = svp::package::create_media_binding(source_path, binding_opts);
  result.blake3_state = svp::package::to_string(
      binding_doc.bindings[0].identity.blake3_state);
  sink->emit(make_stage_completed(ProgressStageId::media_binding));

  if (options.core_only_diagnostic) {
    return write_core_only_svpi(
        options, binding_doc, result.blake3_state,
        "not_generated",
        "SVPI sidecar created in core-only diagnostic mode (semantic pipeline skipped)",
        *sink);
  }

  std::filesystem::path staging_dir;
  if (!options.staging_dir.empty()) {
    staging_dir = options.staging_dir;
  } else {
    staging_dir = std::filesystem::path(options.output_path + ".staging");
  }
  std::filesystem::remove_all(staging_dir);
  std::filesystem::create_directories(staging_dir);

  std::filesystem::path temp_svp_path =
      staging_dir / "interlace_temp.svp";

  BuildPipelineOptions pipeline_opts;
  pipeline_opts.source_path = options.source_path;
  pipeline_opts.probe_json_path = options.probe_json_path;
  pipeline_opts.ffprobe_path = options.ffprobe_path;
  pipeline_opts.ffmpeg_path = options.ffmpeg_path;
  pipeline_opts.output_path = temp_svp_path;
  pipeline_opts.staging_dir = staging_dir;
  pipeline_opts.model_cache_dir = options.model_cache_dir;
  pipeline_opts.stop_after = BuildStage::package_skeleton;
  pipeline_opts.performance = options.performance;
  pipeline_opts.sherpa_lib_path = options.sherpa_lib_path;
  pipeline_opts.allow_fallback_diarization = options.allow_fallback_diarization;
  pipeline_opts.force_single_speaker = options.force_single_speaker;
  pipeline_opts.progress_sink = sink;

  BuildPipeline pipeline;
  auto pipeline_result = pipeline.run(pipeline_opts);

  if (std::filesystem::exists(temp_svp_path)) {
    std::filesystem::remove(temp_svp_path);
  }
  if (std::filesystem::exists(temp_svp_path.string() + ".json")) {
    std::filesystem::remove(temp_svp_path.string() + ".json");
  }

  auto sections = detect_section_states(staging_dir);
  bool has_semantic_content = false;
  for (const auto& key : {"transcript", "timeline", "text", "colors",
                          "entities", "spatial", "relationships", "embeddings"}) {
    if (sections[key]["state"] == "generated") {
      has_semantic_content = true;
      break;
    }
  }

  if (pipeline_result.exit_code != 0 && !has_semantic_content) {
    std::filesystem::remove_all(staging_dir);
    return write_core_only_svpi(
        options, binding_doc, result.blake3_state,
        "blocked",
        "SVPI sidecar created with core-only content (semantic pipeline failed, sections marked blocked)",
        *sink);
  }

  std::string provenance_notes =
      has_semantic_content
          ? "SVPI sidecar created from source media with semantic pipeline output, without embedding primary media bytes"
          : "SVPI sidecar created with core-only content (semantic pipeline produced no section output, sections marked not_generated)";

  return write_svpi_from_staging(
      options, binding_doc, result.blake3_state,
      staging_dir, sections, provenance_notes, *sink);
}

}  // namespace svp::builder
