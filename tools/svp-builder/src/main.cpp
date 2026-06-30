#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/interlace.hpp"
#include "svp/core/version.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/validation/report_json.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>
#include <optional>
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

svp::media::MediaProbe load_or_run_probe(const std::string& source_path,
                                         const std::string& probe_json_path,
                                         const std::string& ffprobe_path) {
  if (!probe_json_path.empty()) {
    return svp::media::load_media_probe_json(probe_json_path);
  }
  return svp::media::probe_media_with_ffprobe(source_path, ffprobe_path);
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
  std::string build_model_cache_dir;
  std::string stop_after = "media-ingest";
  std::string build_sherpa_lib_path;
  bool build_allow_fallback_diarization = false;
  bool build_force_single_speaker = false;

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
  build->add_option("--model-cache", build_model_cache_dir,
                    "Path to SVP model cache directory containing model bundles");
  build->add_option("--stop-after", stop_after,
                    "Supported foundation stages: media-ingest, audio, vision-plan, "
                    "foundation-color, foundation-ocr, package-skeleton");
  build->add_option("--sherpa-lib", build_sherpa_lib_path,
                    "Explicit path to libsherpa-onnx-c-api.dylib for diarization");
  build->add_flag("--allow-fallback-diarization", build_allow_fallback_diarization,
                  "Proceed without diarization if sherpa-onnx is not available. "
                  "Speaker data will be fabricated fallback, not real. NOT RECOMMENDED.");
  build->add_flag("--force-single-speaker", build_force_single_speaker,
                  "Skip Sherpa diarization entirely and emit one speaker segment. "
                  "Use when you know the clip contains only one speaker.");

  // --- interlace subcommand ---
  auto* interlace = app.add_subcommand(
      "interlace", "SVPI sidecar operations: create, validate, inspect, extract, recombine");

  // interlace create
  std::string ic_source;
  std::string ic_out;
  std::string ic_staging;
  std::string ic_model_cache;
  std::string ic_ffprobe = "ffprobe";
  std::string ic_ffmpeg = "ffmpeg";
  std::string ic_probe_json;
  bool ic_no_blake3 = false;

  auto* ic_create = interlace->add_subcommand(
      "create", "Create a .svpi sidecar from source media");
  ic_create->add_option("source", ic_source, "Source media path")->required();
  ic_create->add_option("--out", ic_out, "Output .svpi path")->required();
  ic_create->add_option("--staging-dir", ic_staging, "Staging directory");
  ic_create->add_option("--model-cache", ic_model_cache, "Model cache directory");
  ic_create->add_option("--ffprobe", ic_ffprobe, "ffprobe executable path");
  ic_create->add_option("--ffmpeg", ic_ffmpeg, "ffmpeg executable path");
  ic_create->add_option("--probe-json", ic_probe_json, "Precomputed probe JSON");
  ic_create->add_flag("--no-blake3", ic_no_blake3, "Skip full-file BLAKE3 computation");

  // interlace validate
  std::string iv_svpi;
  std::string iv_media;
  std::string iv_ffprobe = "ffprobe";
  std::string iv_codes = "spec/registries/validation-codes.json";
  bool iv_json = false;

  auto* iv_validate = interlace->add_subcommand(
      "validate", "Validate a .svpi sidecar (structure and optional binding)");
  iv_validate->add_option("svpi", iv_svpi, "SVPI file path")->required();
  iv_validate->add_option("--media", iv_media, "Candidate media file for binding verification");
  iv_validate->add_option("--ffprobe", iv_ffprobe, "ffprobe executable path");
  iv_validate->add_option("--validation-codes", iv_codes, "Validation codes registry path");
  iv_validate->add_flag("--json", iv_json, "Emit JSON output");

  // interlace inspect
  std::string ii_svpi;
  bool ii_json = false;

  auto* ii_inspect = interlace->add_subcommand(
      "inspect", "Inspect a .svpi sidecar");
  ii_inspect->add_option("svpi", ii_svpi, "SVPI file path")->required();
  ii_inspect->add_flag("--json", ii_json, "Emit JSON output");

  // interlace extract
  std::string ie_svp;
  std::string ie_out_dir;
  std::string ie_ffprobe = "ffprobe";
  std::string ie_codes = "spec/registries/validation-codes.json";

  auto* ie_extract = interlace->add_subcommand(
      "extract", "Extract source media and .svpi from a .svp package");
  ie_extract->add_option("svp", ie_svp, "SVP package path")->required();
  ie_extract->add_option("--out-dir", ie_out_dir, "Output directory")->required();
  ie_extract->add_option("--ffprobe", ie_ffprobe, "ffprobe executable path");
  ie_extract->add_option("--validation-codes", ie_codes, "Validation codes registry path");

  // interlace recombine
  std::string ir_media;
  std::string ir_svpi;
  std::string ir_out;
  std::string ir_staging;
  std::string ir_ffprobe = "ffprobe";
  std::string ir_codes = "spec/registries/validation-codes.json";

  auto* ir_recombine = interlace->add_subcommand(
      "recombine", "Recombine source media + .svpi into a .svp package");
  ir_recombine->add_option("media", ir_media, "Source media file")->required();
  ir_recombine->add_option("svpi", ir_svpi, "SVPI sidecar file")->required();
  ir_recombine->add_option("--out", ir_out, "Output .svp path")->required();
  ir_recombine->add_option("--staging-dir", ir_staging, "Staging directory");
  ir_recombine->add_option("--ffprobe", ir_ffprobe, "ffprobe executable path");
  ir_recombine->add_option("--validation-codes", ir_codes, "Validation codes registry path");

  CLI11_PARSE(app, argc, argv);

  try {
    if (*ic_create) {
      svp::builder::InterlaceCreateOptions opts;
      opts.source_path = ic_source;
      opts.output_path = ic_out;
      opts.staging_dir = ic_staging;
      opts.model_cache_dir = ic_model_cache;
      opts.ffprobe_path = ic_ffprobe;
      opts.ffmpeg_path = ic_ffmpeg;
      opts.probe_json_path = ic_probe_json;
      opts.compute_full_blake3 = !ic_no_blake3;

      auto result = svp::builder::interlace_create(opts);
      if (!result.success) {
        std::cerr << "interlace create failed: " << result.error_message << "\n";
        return 1;
      }
      std::cout << "SVPI created: " << result.svpi_path.string() << "\n";
      std::cout << "BLAKE3 state: " << result.blake3_state << "\n";
      std::cout << "Validation status: " << result.binding_state << "\n";
      return 0;
    }

    if (*iv_validate) {
      svp::builder::InterlaceValidateOptions opts;
      opts.svpi_path = iv_svpi;
      opts.media_path = iv_media;
      opts.ffprobe_path = iv_ffprobe;
      opts.validation_codes_path = iv_codes;

      auto result = svp::builder::interlace_validate(opts);

      if (iv_json) {
        nlohmann::json j;
        j["structure_valid"] = result.structure_valid;
        j["binding_attempted"] = result.binding_attempted;
        j["binding_verified"] = result.binding_verified;
        j["binding_state"] = result.binding_state_label;
        j["binding_passing_checks"] = result.binding_passing_checks;
        j["binding_failing_checks"] = result.binding_failing_checks;
        nlohmann::json report_json;
        svp::validation::to_json(report_json, result.validation_report);
        j["validation_report"] = report_json;
        std::cout << j.dump(2) << "\n";
      } else {
        std::cout << "SVPI: " << iv_svpi << "\n";
        std::cout << "Structure valid: " << (result.structure_valid ? "yes" : "no") << "\n";
        if (result.binding_attempted) {
          std::cout << "Binding state: " << result.binding_state_label << "\n";
          if (!result.binding_passing_checks.empty()) {
            std::cout << "Passing checks:\n";
            for (const auto& check : result.binding_passing_checks) {
              std::cout << "  + " << check << "\n";
            }
          }
          if (!result.binding_failing_checks.empty()) {
            std::cout << "Failing checks:\n";
            for (const auto& check : result.binding_failing_checks) {
              std::cout << "  - " << check << "\n";
            }
          }
        } else {
          std::cout << "Binding: not checked (no --media supplied)\n";
        }
        if (!result.validation_report.errors.empty()) {
          std::cout << "Validation errors:\n";
          for (const auto& err : result.validation_report.errors) {
            std::cout << "  " << err.code << ": " << err.message << "\n";
          }
        }
      }
      return result.structure_valid && (!result.binding_attempted || result.binding_verified) ? 0 : 1;
    }

    if (*ii_inspect) {
      svp::builder::InterlaceInspectOptions opts;
      opts.svpi_path = ii_svpi;

      auto result = svp::builder::interlace_inspect(opts);
      if (!result.success) {
        std::cerr << "interlace inspect failed: " << result.error_message << "\n";
        return 1;
      }

      if (ii_json) {
        nlohmann::json j;
        j["artifact_type"] = result.artifact_type;
        j["svpi_version"] = result.svpi_version;
        j["manifest_format"] = result.manifest_format;
        j["entry_count"] = result.entry_count;
        j["root_entries"] = result.root_entries;
        j["binding_id"] = result.binding_id;
        j["binding_contract"] = result.binding_contract;
        j["binding_verification_state"] = result.binding_verification_state;
        j["blake3_state"] = result.blake3_state;
        j["blake3_hash"] = result.blake3_hash;
        j["media_id"] = result.media_id;
        j["size_bytes"] = result.size_bytes;
        j["duration_us"] = result.duration_us;
        j["container_format"] = result.container_format;
        j["original_filename_hint"] = result.original_filename_hint;
        j["has_media_original"] = result.has_media_original;
        j["has_index_sqlite"] = result.has_index_sqlite;
        j["has_index_manifest"] = result.has_index_manifest;
        j["has_provenance"] = result.has_provenance;
        j["recombination_ready"] = result.recombination_ready;
        j["section_states"] = result.section_states;
        std::cout << j.dump(2) << "\n";
      } else {
        std::cout << "Artifact type: " << result.artifact_type << "\n";
        std::cout << "SVPI version: " << result.svpi_version << "\n";
        std::cout << "Manifest format: " << result.manifest_format << "\n";
        std::cout << "Entry count: " << result.entry_count << "\n";
        std::cout << "Binding ID: " << result.binding_id << "\n";
        std::cout << "Binding contract: " << result.binding_contract << "\n";
        std::cout << "Binding verification state: " << result.binding_verification_state << "\n";
        std::cout << "BLAKE3 state: " << result.blake3_state << "\n";
        if (!result.blake3_hash.empty()) {
          std::cout << "BLAKE3 hash: " << result.blake3_hash << "\n";
        }
        std::cout << "Media ID: " << result.media_id << "\n";
        std::cout << "Size bytes: " << result.size_bytes << "\n";
        std::cout << "Duration us: " << result.duration_us << "\n";
        std::cout << "Container format: " << result.container_format << "\n";
        std::cout << "Original filename hint: " << result.original_filename_hint << "\n";
        std::cout << "Has media/original: " << (result.has_media_original ? "yes" : "no") << "\n";
        std::cout << "Has index.sqlite: " << (result.has_index_sqlite ? "yes" : "no") << "\n";
        std::cout << "Has index_manifest.json: " << (result.has_index_manifest ? "yes" : "no") << "\n";
        std::cout << "Has provenance: " << (result.has_provenance ? "yes" : "no") << "\n";
        std::cout << "Recombination ready: " << (result.recombination_ready ? "yes" : "no") << "\n";
      }
      return 0;
    }

    if (*ie_extract) {
      svp::builder::InterlaceExtractOptions opts;
      opts.svp_path = ie_svp;
      opts.out_dir = ie_out_dir;
      opts.ffprobe_path = ie_ffprobe;
      opts.validation_codes_path = ie_codes;

      auto result = svp::builder::interlace_extract(opts);
      if (!result.success) {
        std::cerr << "interlace extract failed: " << result.error_message << "\n";
        return 1;
      }
      std::cout << "Extracted media: " << result.extracted_media_path.string() << "\n";
      std::cout << "Extracted SVPI: " << result.extracted_svpi_path.string() << "\n";
      return 0;
    }

    if (*ir_recombine) {
      svp::builder::InterlaceRecombineOptions opts;
      opts.media_path = ir_media;
      opts.svpi_path = ir_svpi;
      opts.output_path = ir_out;
      opts.staging_dir = ir_staging;
      opts.ffprobe_path = ir_ffprobe;
      opts.validation_codes_path = ir_codes;

      auto result = svp::builder::interlace_recombine(opts);
      if (!result.success) {
        std::cerr << "interlace recombine failed: " << result.error_message << "\n";
        return 1;
      }
      std::cout << "Recombined SVP: " << result.svp_path.string() << "\n";
      std::cout << "Binding state: " << result.binding_state_label << "\n";
      const int svp_exit = svp::validation::exit_code(result.validation_report);
      std::cout << "SVP validation exit code: " << svp_exit << "\n";
      return svp_exit;
    }

    if (*probe) {
      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(
              probe_source_path,
              load_or_run_probe(probe_source_path, probe_json_path,
                                probe_ffprobe_path));
      if (probe_json_output) {
        std::cout << svp::media::media_ingest_plan_to_json(plan).dump(2) << "\n";
      } else {
        print_plan_summary(plan);
      }
      return 0;
    }

    if (*build) {
      const std::optional<svp::builder::BuildStage> parsed_stage =
          svp::builder::parse_build_stage(stop_after);
      if (!parsed_stage.has_value()) {
        std::cerr << "svp-builder build currently supports --stop-after media-ingest, audio, "
                     "vision-plan, foundation-color, foundation-ocr, or package-skeleton\n";
        return 2;
      }

      svp::builder::BuildPipelineOptions options;
      options.source_path = build_source_path;
      options.probe_json_path = build_probe_json_path;
      options.ffprobe_path = build_ffprobe_path;
      options.ffmpeg_path = build_ffmpeg_path;
      options.output_path = build_output_path;
      options.staging_dir = build_staging_dir;
      options.model_cache_dir = build_model_cache_dir;
      options.stop_after = *parsed_stage;
      options.sherpa_lib_path = build_sherpa_lib_path;
      options.allow_fallback_diarization = build_allow_fallback_diarization;
      options.force_single_speaker = build_force_single_speaker;

      const svp::builder::BuildPipelineResult result =
          svp::builder::BuildPipeline{}.run(options);
      return result.exit_code;
    }
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
