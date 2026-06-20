#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/vad_execution_boundary.hpp"
#include "svp/core/version.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/foundation_color_staging.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/observation_pipeline_plan.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/package/relationship_provenance_writer.hpp"
#include "svp/package/spatial_embedding_placeholders.hpp"
#include "svp/package/validation_report_storage.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/validation/validator.hpp"
#include <ctime>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_plan_summary(const svp::media::MediaIngestPlan& plan) {
  std::cout << "SVP builder media ingest foundation\n";
  std::cout << "Source: " << plan.source_path.string() << "\n";
  std::cout << "Primary video stream: " << plan.primary_video_stream.id << "\n";
  std::cout << "Stored dimensions: " << plan.primary_video_stream.width << "x"
            << plan.primary_video_stream.height << "\n";
  std::cout << "Display aspect ratio: "
            << plan.canonical_raster.display.display_aspect_ratio << "\n";
  std::cout << "Canonical analysis raster: " << plan.canonical_raster.width << "x"
            << plan.canonical_raster.height << "\n";
  std::cout << "Audio streams: " << plan.probe.audio_streams.size() << "\n";
  std::cout << "Package writer: not run for this foundation command\n";
}

void write_json_file(const std::filesystem::path& output_path,
                     const nlohmann::json& value) {
  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(output_path);
  if (!output) {
    throw std::runtime_error("unable to open output path: " + output_path.string());
  }
  output << value.dump(2) << "\n";
}

void write_jsonl_file(const std::filesystem::path& output_path,
                      const nlohmann::json& records) {
  if (!records.is_array()) {
    throw std::runtime_error("jsonl staging records must be an array");
  }

  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(output_path);
  if (!output) {
    throw std::runtime_error("unable to open output path: " + output_path.string());
  }

  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

void append_jsonl_file(const std::filesystem::path& output_path,
                       const nlohmann::json& records) {
  if (!records.is_array()) {
    throw std::runtime_error("jsonl staging records must be an array");
  }

  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(output_path, std::ios::app);
  if (!output) {
    throw std::runtime_error("unable to open output path: " + output_path.string());
  }

  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

void write_foundation_color_staging_files(
    const std::filesystem::path& staging_dir,
    const svp::vision::FoundationColorStagingArtifact& artifact) {
  const nlohmann::json color_observations =
      svp::vision::color_observation_records_to_jsonl_array(
          artifact.records.records);
  write_jsonl_file(staging_dir / "colors" / "color_observations.jsonl",
                   color_observations);
  write_json_file(staging_dir / "colors" / "color_summary.json",
                  artifact.records.color_summary);
  write_json_file(staging_dir / "colors" / "color_absence.json",
                  artifact.records.color_absence);
  append_jsonl_file(staging_dir / "provenance" / "processors.jsonl",
                   nlohmann::json::array({artifact.processor_provenance}));
}

void write_foundation_ocr_staging_files(
    const std::filesystem::path& staging_dir,
    const svp::vision::FoundationOcrStagingArtifact& artifact) {
  nlohmann::json regions_arr = nlohmann::json::array();
  for (const auto& reg : artifact.text_regions) {
    regions_arr.push_back(svp::vision::text_region_to_json(reg));
  }
  nlohmann::json obs_arr = nlohmann::json::array();
  for (const auto& obs : artifact.text_observations) {
    obs_arr.push_back(svp::vision::text_observation_to_json(obs));
  }
  nlohmann::json num_arr = nlohmann::json::array();
  for (const auto& num : artifact.numeric_values) {
    num_arr.push_back(svp::vision::numeric_value_to_json(num));
  }

  write_jsonl_file(staging_dir / "text" / "text_regions.jsonl", regions_arr);
  write_jsonl_file(staging_dir / "text" / "text_observations.jsonl", obs_arr);
  write_jsonl_file(staging_dir / "text" / "numeric_values.jsonl", num_arr);
  write_json_file(staging_dir / "text" / "text_absence.json",
                  svp::vision::text_absence_to_json(artifact.text_absence));

  nlohmann::json processors_arr = nlohmann::json::array();
  for (const auto& proc : artifact.processors) {
    processors_arr.push_back(proc);
  }
  append_jsonl_file(staging_dir / "provenance" / "processors.jsonl", processors_arr);
}

svp::media::MediaProbe load_or_run_probe(const std::string& source_path,
                                         const std::string& probe_json_path,
                                         const std::string& ffprobe_path) {
  if (!probe_json_path.empty()) {
    return svp::media::load_media_probe_json(probe_json_path);
  }
  return svp::media::probe_media_with_ffprobe(source_path, ffprobe_path);
}

bool executable_exists(const std::filesystem::path& executable_path) {
  if (executable_path.empty()) {
    return false;
  }
  if (executable_path.has_parent_path()) {
    return std::filesystem::exists(executable_path);
  }

  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return false;
  }

  std::string paths(path_env);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = paths.find(':', start);
    const std::string entry =
        paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty() && std::filesystem::exists(std::filesystem::path(entry) / executable_path)) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return false;
}

std::filesystem::path default_staging_dir_for_output(
    const std::filesystem::path& output_path) {
  return std::filesystem::path(output_path.string() + ".staging");
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"SVP builder"};
  app.set_version_flag("--version", svp::core::tool_version_label("svp-builder"));
  app.require_subcommand(0, 1);

  std::string probe_source_path;
  std::string probe_json_path;
  std::string probe_ffprobe_path = "ffprobe";
  bool probe_json_output = false;

  auto* probe = app.add_subcommand(
      "probe", "Read deterministic media probe metadata and compute SVP timing data");
  probe->add_option("source", probe_source_path, "Source media path")->required();
  probe->add_option("--probe-json", probe_json_path,
                    "Precomputed media probe JSON; skips running ffprobe");
  probe->add_option("--ffprobe", probe_ffprobe_path, "ffprobe executable path");
  probe->add_flag("--json", probe_json_output, "Emit JSON");

  std::string build_source_path;
  std::string build_probe_json_path;
  std::string build_ffprobe_path = "ffprobe";
  std::string build_ffmpeg_path = "ffmpeg";
  std::string build_output_path;
  std::string build_staging_dir;
  std::string stop_after = "media-ingest";

  auto* build = app.add_subcommand(
      "build", "Write an honest builder foundation JSON artifact");
  build->add_option("source", build_source_path, "Source media path")->required();
  build->add_option("--probe-json", build_probe_json_path,
                    "Precomputed media probe JSON; skips running ffprobe");
  build->add_option("--ffprobe", build_ffprobe_path, "ffprobe executable path");
  build->add_option("--ffmpeg", build_ffmpeg_path, "ffmpeg executable path");
  build->add_option("--out", build_output_path,
                    "Output path for the builder foundation JSON")
      ->required();
  build->add_option("--staging-dir", build_staging_dir,
                    "Directory for staged builder outputs");
  build->add_option("--stop-after", stop_after,
                    "Supported foundation stages: media-ingest, audio, vision-plan, "
                    "foundation-color, foundation-ocr, package-skeleton");

  CLI11_PARSE(app, argc, argv);

  try {
    if (*probe) {
      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(probe_source_path,
                                              load_or_run_probe(probe_source_path,
                                                                probe_json_path,
                                                                probe_ffprobe_path));
      if (probe_json_output) {
        std::cout << svp::media::media_ingest_plan_to_json(plan).dump(2) << "\n";
      } else {
        print_plan_summary(plan);
      }
      return 0;
    }

    if (*build) {
      if (stop_after != "media-ingest" && stop_after != "audio" &&
          stop_after != "vision-plan" && stop_after != "foundation-color" &&
          stop_after != "foundation-ocr" && stop_after != "package-skeleton") {
        std::cerr << "svp-builder build currently supports --stop-after media-ingest, audio, "
                     "vision-plan, foundation-color, foundation-ocr, or package-skeleton\n";
        return 2;
      }

      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(build_source_path,
                                              load_or_run_probe(build_source_path,
                                                                build_probe_json_path,
                                                                build_ffprobe_path));
      nlohmann::json output = svp::media::media_ingest_plan_to_json(plan);

      const std::filesystem::path staging_dir =
          build_staging_dir.empty()
              ? default_staging_dir_for_output(build_output_path)
              : std::filesystem::path(build_staging_dir);
      const bool model_runtime_available = svp::models::OnnxSession::is_available();

      if (stop_after == "audio" || stop_after == "package-skeleton") {
        const svp::audio::AudioStagePlan audio_plan =
            svp::audio::build_audio_stage_plan(build_source_path,
                                               plan.probe,
                                               executable_exists(build_ffmpeg_path),
                                               build_ffmpeg_path,
                                               model_runtime_available);
        nlohmann::json audio_json = svp::audio::audio_stage_plan_to_json(audio_plan);
        const svp::audio::AudioExtractionRun extraction_run =
            svp::audio::execute_audio_extraction_plan(audio_plan.extraction_plan,
                                                      staging_dir);
        nlohmann::json extraction_run_json =
            svp::audio::audio_extraction_run_to_json(extraction_run);
        const svp::audio::VadExecutionBoundary vad_boundary =
            svp::audio::build_vad_execution_boundary(audio_plan.vad_task_plan,
                                                     extraction_run.analysis_audio_written,
                                                     extraction_run.waveform_written,
                                                     model_runtime_available);
        const svp::audio::VadExecutionBoundary executed_boundary =
            svp::audio::execute_vad_boundary(vad_boundary, staging_dir);

        audio_json["audio_extraction"]["execution"] = extraction_run_json;
        audio_json["audio_extraction"]["extraction_run"] =
            extraction_run.extraction_run;
        audio_json["audio_extraction"]["original_streams_written"] =
            extraction_run.original_streams_written;
        audio_json["audio_extraction"]["analysis_audio_written"] =
            extraction_run.analysis_audio_written;
        audio_json["audio_extraction"]["audio_absence_written"] =
            extraction_run.audio_absence_written;
        audio_json["audio_extraction"]["waveform_written"] =
            extraction_run.waveform_written;
        audio_json["audio_extraction"]["processor_provenance_written"] =
            extraction_run.processor_provenance_written;
        for (const std::string& blocker : extraction_run.blockers) {
          audio_json["blockers"].push_back(blocker);
        }
        for (const std::string& blocker : executed_boundary.blockers) {
          if (std::find(audio_json["blockers"].begin(), audio_json["blockers"].end(), blocker) == audio_json["blockers"].end()) {
            audio_json["blockers"].push_back(blocker);
          }
        }
        audio_json["vad_execution_boundary"] =
            svp::audio::vad_execution_boundary_to_json(executed_boundary);
        output["audio_foundation"] = audio_json;
      }

      if (stop_after == "vision-plan") {
        const svp::vision::VisionObservationPipelinePlan vision_plan =
            svp::vision::build_vision_observation_pipeline_plan(plan);
        output["vision_observation_pipeline"] =
            svp::vision::vision_observation_pipeline_plan_to_json(vision_plan);
      }

      if (stop_after == "foundation-color" || stop_after == "package-skeleton") {
        const svp::vision::FoundationColorStagingArtifact color_artifact =
            svp::vision::build_real_frame_color_staging_artifact(
                plan, build_ffmpeg_path);
        write_foundation_color_staging_files(staging_dir, color_artifact);

        nlohmann::json color_json =
            svp::vision::foundation_color_staging_artifact_to_json(color_artifact);
        color_json["staging_paths"] = {
            {"color_observations_jsonl",
             (staging_dir / "colors" / "color_observations.jsonl").string()},
            {"color_summary_json",
             (staging_dir / "colors" / "color_summary.json").string()},
            {"color_absence_json",
             (staging_dir / "colors" / "color_absence.json").string()},
            {"processors_jsonl",
             (staging_dir / "provenance" / "processors.jsonl").string()},
        };
        output["foundation_color_staging"] = color_json;
      }

      if (stop_after == "foundation-ocr" || stop_after == "package-skeleton") {
        const svp::vision::FoundationOcrStagingArtifact ocr_artifact =
            svp::vision::build_real_ocr_staging_artifact(
                plan, model_runtime_available);
        write_foundation_ocr_staging_files(staging_dir, ocr_artifact);

        nlohmann::json ocr_json =
            svp::vision::foundation_ocr_staging_artifact_to_json(ocr_artifact);
        ocr_json["staging_paths"] = {
            {"text_regions_jsonl",
             (staging_dir / "text" / "text_regions.jsonl").string()},
            {"text_observations_jsonl",
             (staging_dir / "text" / "text_observations.jsonl").string()},
            {"numeric_values_jsonl",
             (staging_dir / "text" / "numeric_values.jsonl").string()},
            {"text_absence_json",
             (staging_dir / "text" / "text_absence.json").string()},
            {"processors_jsonl",
             (staging_dir / "provenance" / "processors.jsonl").string()},
        };
        output["foundation_ocr_staging"] = ocr_json;
      }

      bool package_written = false;
      bool validator_passes = false;
      bool validation_report_stored = false;
      int validator_exit_code = -1;
      nlohmann::json validation_report_json = nlohmann::json::object();
      std::filesystem::path package_path;
      std::filesystem::path json_out_path = build_output_path;

      if (stop_after == "package-skeleton") {
        if (std::filesystem::path(build_output_path).extension() == ".svp") {
          package_path = build_output_path;
          json_out_path = build_output_path + ".json";
        } else {
          package_path = std::filesystem::path(build_output_path).replace_extension(".svp");
        }

        std::time_t now = std::time(nullptr);
        char buf[100];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        std::string created_utc(buf);

        nlohmann::json manifest_json = {
          {"svp_version", "1.0-rc.2"},
          {"package_id", "svp_" + std::filesystem::path(build_source_path).stem().string() + "_pkg"},
          {"created_utc", created_utc},
          {"primary_media_id", "media_000001"},
          {"timebase", {
            {"unit", "microseconds"},
            {"origin", "primary_presentation_start"},
            {"source_timebase_mode", "exact_rational"},
            {"rounding", "round_half_to_even"}
          }},
          {"canonical_analysis_raster", output["canonical_analysis_raster"]},
          {"binary_block_format", {
            {"magic", "SVPB"},
            {"version", 1},
            {"header_size", 160},
            {"compression", "zstd"}
          }},
          {"hashes", {
            {"algorithm", "blake3"},
            {"digest_bytes", 32},
            {"encoding", "lowercase_hex"},
            {"manifest_excluded", true}
          }},
          {"required_sections", {
            {"media", true},
            {"transcript", true},
            {"timeline", true},
            {"entities", true},
            {"spatial", true},
            {"relationships", true},
            {"embeddings", true},
            {"index", true},
            {"provenance", true}
          }}
        };

        const svp::package::RelationshipProvenanceWriteSummary relationship_summary =
            svp::package::write_relationships_and_provenance(staging_dir);
        output["package_relationships_provenance"] =
            svp::package::relationship_provenance_write_summary_to_json(
                relationship_summary);

        // Write honest spatial/embedding placeholder entries
        const svp::package::SpatialEmbeddingPlaceholderSummary placeholder_summary =
            svp::package::write_spatial_and_embedding_placeholders(staging_dir);
        output["spatial_embedding_placeholders"] =
            svp::package::spatial_embedding_placeholder_summary_to_json(
                placeholder_summary);

        // Generate SQLite index foundation and manifest
        if (!svp::package::write_index_foundation(staging_dir, manifest_json)) {
          std::cerr << "Warning: failed to write SQLite index foundation.\n";
        }

        // First package write (without validation report)
        package_written = svp::package::write_package_skeleton(
            package_path, staging_dir, build_source_path, manifest_json);

        if (package_written) {
          svp::validation::ValidatorOptions validator_opts;
          validator_opts.validation_codes_path = "spec/registries/validation-codes.json";

          // Run validator on first package
          auto first_report = svp::validation::validate_package(package_path, validator_opts);
          validation_report_json = first_report;

          // Store validation report in staging for second package write
          validation_report_stored = svp::package::write_validation_report_to_staging(
              staging_dir, validation_report_json);

          if (validation_report_stored) {
            // Re-package with validation report included
            package_written = svp::package::write_package_skeleton(
                package_path, staging_dir, build_source_path, manifest_json);
          } else {
            package_written = false;
          }

          if (package_written) {
            // Run validator on final package
            auto final_report = svp::validation::validate_package(package_path, validator_opts);
            validator_exit_code = svp::validation::exit_code(final_report);
            validator_passes = (validator_exit_code == 0);
            validation_report_json = final_report;
          }
        }
      }

      output["builder_command"] = {
          {"command", "build"},
          {"stop_after", stop_after},
          {"valid_svp_package_written", validator_passes},
      };

      if (stop_after == "package-skeleton") {
        output["builder_command"]["package_path"] = package_path.string();
        output["builder_command"]["validator"] = {
          {"exit_code", validator_exit_code},
          {"validation_report_stored", validation_report_stored},
          {"validator_proven_valid", validator_passes},
          {"report", validation_report_json}
        };
        if (validation_report_stored) {
          output["builder_command"]["validator"]["validation_report_path"] =
              "provenance/validation.json";
        }
      }

      write_json_file(json_out_path, output);
      std::cout << "Wrote builder foundation JSON: " << json_out_path << "\n";

      if (stop_after == "audio") {
        std::cout << "Audio task plan only; no transcription or diarization was generated.\n";
      }
      if (stop_after == "vision-plan") {
        std::cout << "Vision/OCR/color task plan only; no observations were generated.\n";
      }
      if (stop_after == "foundation-color") {
        const bool real_run =
            output.at("foundation_color_staging").at("manifest").value(
                "real_media_frame_decoding_run", false);
        std::cout << "Staged foundation color observations under: "
                  << staging_dir << "\n";
        if (real_run) {
          std::cout << "Real media frames were decoded for the color staging artifact.\n";
        } else {
          std::cout << "No real media frames were decoded; synthetic fallback was used.\n";
        }
      }
      if (stop_after == "foundation-ocr") {
        const bool ocr_detection_run =
            output.at("foundation_ocr_staging").at("manifest").value(
                "ocr_detection_run", false);
        std::cout << "Staged foundation OCR observations under: "
                  << staging_dir << "\n";
        if (ocr_detection_run) {
          std::cout << "Real media frames were processed for the OCR staging artifact.\n";
        } else {
          std::cout << "No real OCR models were executed; honest absence was reported.\n";
        }
      }
      if (stop_after == "package-skeleton") {
        std::cout << "Staged foundation files under: " << staging_dir << "\n";
        std::cout << "Relationships written: "
                  << output.at("package_relationships_provenance").at("relationships_written")
                  << "\n";
        std::cout << "Processor provenance records written: "
                  << output.at("package_relationships_provenance").at("processors_written")
                  << "\n";
        std::cout << "Spatial/embedding placeholders written: "
                  << output.at("spatial_embedding_placeholders").at("depth_index_written")
                  << " depth index, "
                  << output.at("spatial_embedding_placeholders").at("masks_blocks_written")
                  << " masks blocks, "
                  << output.at("spatial_embedding_placeholders").at("embedding_sets_written")
                  << " embedding sets\n";
        std::cout << "Wrote skeleton .svp package to: " << package_path << "\n";
        if (validation_report_stored) {
          std::cout << "Validation report stored at: provenance/validation.json\n";
        } else {
          std::cout << "Validation report storage FAILED\n";
        }
        std::cout << "Validator exit code: " << validator_exit_code << "\n";
        if (validator_passes) {
          std::cout << "Package validation: SUCCESS\n";
        } else {
          std::cout << "Package validation: INCOMPLETE/INVALID (Expected for skeleton package)\n";
        }
      } else {
        std::cout << "No .svp package was created by this foundation command.\n";
      }
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
