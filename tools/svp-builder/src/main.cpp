#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/interlace.hpp"
#include "svp/builder/interlace_batch.hpp"
#include "svp/builder/progress_renderer.hpp"
#include "svp/core/version.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/validation/report_json.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>

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
  std::string build_progress_mode = "auto";
  bool build_quiet = false;
  bool build_verbose = false;

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
  build->add_option("--progress", build_progress_mode,
                    "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  build->add_flag("--quiet", build_quiet,
                  "Suppress progress output; print only final success/failure");
  build->add_flag("--verbose", build_verbose,
                  "Include detailed diagnostics and full validation findings");

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
  std::string ic_sherpa_lib;
  bool ic_no_blake3 = false;
  bool ic_core_only = false;
  bool ic_allow_fallback = false;
  bool ic_force_single = false;
  std::string ic_progress_mode = "auto";
  bool ic_quiet = false;

  auto* ic_create = interlace->add_subcommand(
      "create", "Create a .svpi sidecar from source media");
  ic_create->add_option("source", ic_source, "Source media path")->required();
  ic_create->add_option("--out", ic_out, "Output .svpi path")->required();
  ic_create->add_option("--staging-dir", ic_staging, "Staging directory");
  ic_create->add_option("--model-cache", ic_model_cache, "Model cache directory");
  ic_create->add_option("--ffprobe", ic_ffprobe, "ffprobe executable path");
  ic_create->add_option("--ffmpeg", ic_ffmpeg, "ffmpeg executable path");
  ic_create->add_option("--probe-json", ic_probe_json, "Precomputed probe JSON");
  ic_create->add_option("--sherpa-lib", ic_sherpa_lib, "Path to sherpa-onnx shared library");
  ic_create->add_flag("--no-blake3", ic_no_blake3, "Skip full-file BLAKE3 computation");
  ic_create->add_flag("--core-only-diagnostic", ic_core_only,
      "Emit core-only SVPI without running semantic pipeline (diagnostic mode)");
  ic_create->add_flag("--allow-fallback-diarization", ic_allow_fallback,
      "Allow fallback diarization when sherpa-onnx is unavailable");
  ic_create->add_flag("--force-single-speaker", ic_force_single,
      "Force single-speaker diarization");
  ic_create->add_option("--progress", ic_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ic_create->add_flag("--quiet", ic_quiet,
      "Suppress progress output; print only final success/failure");

  // interlace validate
  std::string iv_svpi;
  std::string iv_media;
  std::string iv_ffprobe = "ffprobe";
  std::string iv_codes = "spec/registries/validation-codes.json";
  bool iv_json = false;
  std::string iv_progress_mode = "auto";
  bool iv_quiet = false;

  auto* iv_validate = interlace->add_subcommand(
      "validate", "Validate a .svpi sidecar (structure and optional binding)");
  iv_validate->add_option("svpi", iv_svpi, "SVPI file path")->required();
  iv_validate->add_option("--media", iv_media, "Candidate media file for binding verification");
  iv_validate->add_option("--ffprobe", iv_ffprobe, "ffprobe executable path");
  iv_validate->add_option("--validation-codes", iv_codes, "Validation codes registry path");
  iv_validate->add_flag("--json", iv_json, "Emit JSON output");
  iv_validate->add_option("--progress", iv_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  iv_validate->add_flag("--quiet", iv_quiet,
      "Suppress progress output; print only final success/failure");

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
  std::string ie_progress_mode = "auto";
  bool ie_quiet = false;

  auto* ie_extract = interlace->add_subcommand(
      "extract", "Extract source media and .svpi from a .svp package");
  ie_extract->add_option("svp", ie_svp, "SVP package path")->required();
  ie_extract->add_option("--out-dir", ie_out_dir, "Output directory")->required();
  ie_extract->add_option("--ffprobe", ie_ffprobe, "ffprobe executable path");
  ie_extract->add_option("--validation-codes", ie_codes, "Validation codes registry path");
  ie_extract->add_option("--progress", ie_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ie_extract->add_flag("--quiet", ie_quiet,
      "Suppress progress output; print only final success/failure");

  // interlace recombine
  std::string ir_media;
  std::string ir_svpi;
  std::string ir_out;
  std::string ir_staging;
  std::string ir_ffprobe = "ffprobe";
  std::string ir_codes = "spec/registries/validation-codes.json";
  std::string ir_progress_mode = "auto";
  bool ir_quiet = false;

  auto* ir_recombine = interlace->add_subcommand(
      "recombine", "Recombine source media + .svpi into a .svp package");
  ir_recombine->add_option("media", ir_media, "Source media file")->required();
  ir_recombine->add_option("svpi", ir_svpi, "SVPI sidecar file")->required();
  ir_recombine->add_option("--out", ir_out, "Output .svp path")->required();
  ir_recombine->add_option("--staging-dir", ir_staging, "Staging directory");
  ir_recombine->add_option("--ffprobe", ir_ffprobe, "ffprobe executable path");
  ir_recombine->add_option("--validation-codes", ir_codes, "Validation codes registry path");
  ir_recombine->add_option("--progress", ir_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ir_recombine->add_flag("--quiet", ir_quiet,
      "Suppress progress output; print only final success/failure");

  // interlace create-batch
  std::string cb_source_dir;
  std::string cb_out_dir;
  std::string cb_model_cache;
  std::string cb_ffprobe = "ffprobe";
  std::string cb_ffmpeg = "ffmpeg";
  std::string cb_staging;
  std::string cb_sherpa_lib;
  std::string cb_visibility = "visible";
  bool cb_recursive = false;
  bool cb_no_blake3 = false;
  bool cb_replace_mismatched = false;
  bool cb_json = false;
  bool cb_core_only = false;
  bool cb_allow_fallback = false;
  bool cb_force_single = false;
  std::string cb_progress_mode = "auto";
  bool cb_quiet = false;

  auto* cb_create_batch = interlace->add_subcommand(
      "create-batch", "Create .svpi sidecars for all supported videos in a directory");
  cb_create_batch->add_option("source-dir", cb_source_dir, "Directory containing source videos")->required();
  cb_create_batch->add_option("--out-dir", cb_out_dir, "Output directory (default: same as source)");
  cb_create_batch->add_option("--model-cache", cb_model_cache, "Model cache directory");
  cb_create_batch->add_option("--ffprobe", cb_ffprobe, "ffprobe executable path");
  cb_create_batch->add_option("--ffmpeg", cb_ffmpeg, "ffmpeg executable path");
  cb_create_batch->add_option("--staging-dir", cb_staging, "Staging directory");
  cb_create_batch->add_option("--sherpa-lib", cb_sherpa_lib, "Path to sherpa-onnx shared library");
  cb_create_batch->add_option("--sidecar-visibility", cb_visibility,
      "Sidecar naming: visible, hidden, managed-dir")
      ->check(CLI::IsMember({"visible", "hidden", "managed-dir"}));
  cb_create_batch->add_flag("--recursive", cb_recursive, "Search subdirectories recursively");
  cb_create_batch->add_flag("--no-blake3", cb_no_blake3, "Skip full-file BLAKE3 computation");
  cb_create_batch->add_flag("--replace-mismatched", cb_replace_mismatched,
      "Replace existing sidecars that fail binding verification");
  cb_create_batch->add_flag("--json", cb_json, "Emit JSON summary report");
  cb_create_batch->add_flag("--core-only-diagnostic", cb_core_only,
      "Emit core-only SVPI without running semantic pipeline (diagnostic mode)");
  cb_create_batch->add_flag("--allow-fallback-diarization", cb_allow_fallback,
      "Allow fallback diarization when sherpa-onnx is unavailable");
  cb_create_batch->add_flag("--force-single-speaker", cb_force_single,
      "Force single-speaker diarization");
  cb_create_batch->add_option("--progress", cb_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  cb_create_batch->add_flag("--quiet", cb_quiet,
      "Suppress progress output; print only final success/failure");

  // interlace scan
  std::string sc_source_dir;
  bool sc_recursive = false;
  bool sc_json = false;
  std::string sc_ffprobe = "ffprobe";
  std::string sc_codes = "spec/registries/validation-codes.json";

  auto* sc_scan = interlace->add_subcommand(
      "scan", "Discover media/SVPI pairs in a directory");
  sc_scan->add_option("source-dir", sc_source_dir, "Directory to scan")->required();
  sc_scan->add_flag("--recursive", sc_recursive, "Search subdirectories recursively");
  sc_scan->add_flag("--json", sc_json, "Emit JSON output");
  sc_scan->add_option("--ffprobe", sc_ffprobe, "ffprobe executable path");
  sc_scan->add_option("--validation-codes", sc_codes, "Validation codes registry path");

  // interlace validate-batch
  std::string vb_source_dir;
  bool vb_recursive = false;
  bool vb_json = false;
  std::string vb_ffprobe = "ffprobe";
  std::string vb_codes = "spec/registries/validation-codes.json";
  std::string vb_progress_mode = "auto";
  bool vb_quiet = false;

  auto* vb_validate_batch = interlace->add_subcommand(
      "validate-batch", "Validate all .svpi files in a directory");
  vb_validate_batch->add_option("source-dir", vb_source_dir, "Directory to validate")->required();
  vb_validate_batch->add_flag("--recursive", vb_recursive, "Search subdirectories recursively");
  vb_validate_batch->add_flag("--json", vb_json, "Emit JSON output");
  vb_validate_batch->add_option("--ffprobe", vb_ffprobe, "ffprobe executable path");
  vb_validate_batch->add_option("--validation-codes", vb_codes, "Validation codes registry path");
  vb_validate_batch->add_option("--progress", vb_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  vb_validate_batch->add_flag("--quiet", vb_quiet,
      "Suppress progress output; print only final success/failure");

  // interlace complete-identity
  std::string ci_svpi;
  std::string ci_media;
  std::string ci_ffprobe = "ffprobe";
  std::string ci_codes = "spec/registries/validation-codes.json";
  std::string ci_progress_mode = "auto";
  bool ci_quiet = false;

  auto* ci_complete = interlace->add_subcommand(
      "complete-identity", "Complete pending full-file BLAKE3 identity for an SVPI");
  ci_complete->add_option("svpi", ci_svpi, "SVPI file path")->required();
  ci_complete->add_option("--media", ci_media, "Source media file")->required();
  ci_complete->add_option("--ffprobe", ci_ffprobe, "ffprobe executable path");
  ci_complete->add_option("--validation-codes", ci_codes, "Validation codes registry path");
  ci_complete->add_option("--progress", ci_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ci_complete->add_flag("--quiet", ci_quiet,
      "Suppress progress output; print only final success/failure");

  // interlace complete-identity-batch
  std::string cib_source_dir;
  bool cib_recursive = false;
  bool cib_json = false;
  std::string cib_ffprobe = "ffprobe";
  std::string cib_codes = "spec/registries/validation-codes.json";
  std::string cib_progress_mode = "auto";
  bool cib_quiet = false;

  auto* cib_complete_batch = interlace->add_subcommand(
      "complete-identity-batch", "Complete pending BLAKE3 identity for all SVPI files in a directory");
  cib_complete_batch->add_option("source-dir", cib_source_dir, "Directory containing SVPI files")->required();
  cib_complete_batch->add_flag("--recursive", cib_recursive, "Search subdirectories recursively");
  cib_complete_batch->add_flag("--json", cib_json, "Emit JSON output");
  cib_complete_batch->add_option("--ffprobe", cib_ffprobe, "ffprobe executable path");
  cib_complete_batch->add_option("--validation-codes", cib_codes, "Validation codes registry path");
  cib_complete_batch->add_option("--progress", cib_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  cib_complete_batch->add_flag("--quiet", cib_quiet,
      "Suppress progress output; print only final success/failure");

  CLI11_PARSE(app, argc, argv);

  try {
    auto resolve_interlace_sink = [](const std::string& mode_str,
                                     bool quiet,
                                     CLI::App* subcmd)
        -> std::shared_ptr<svp::builder::BuildProgressSink> {
      auto progress_opt = subcmd->get_option("--progress");
      const bool progress_explicitly_set =
          progress_opt && progress_opt->count() > 0;

      std::optional<svp::builder::ProgressMode> resolved_mode;
      if (quiet) {
        if (progress_explicitly_set && mode_str == "json") {
          resolved_mode = svp::builder::ProgressMode::json;
        } else {
          resolved_mode = svp::builder::ProgressMode::none;
        }
      } else {
        resolved_mode = svp::builder::parse_progress_mode(mode_str);
      }

      if (!resolved_mode) {
        std::cerr << "svp-builder: invalid --progress value: " << mode_str << "\n";
        return nullptr;
      }

      const bool stderr_is_tty = isatty(fileno(stderr)) != 0;
      return svp::builder::make_progress_sink(
          *resolved_mode, std::cerr, stderr_is_tty);
    };

    if (*ic_create) {
      auto sink = resolve_interlace_sink(ic_progress_mode, ic_quiet, ic_create);
      if (!sink) return 2;

      svp::builder::InterlaceCreateOptions opts;
      opts.source_path = ic_source;
      opts.output_path = ic_out;
      opts.staging_dir = ic_staging;
      opts.model_cache_dir = ic_model_cache;
      opts.ffprobe_path = ic_ffprobe;
      opts.ffmpeg_path = ic_ffmpeg;
      opts.probe_json_path = ic_probe_json;
      opts.sherpa_lib_path = ic_sherpa_lib;
      opts.compute_full_blake3 = !ic_no_blake3;
      opts.core_only_diagnostic = ic_core_only;
      opts.allow_fallback_diarization = ic_allow_fallback;
      opts.force_single_speaker = ic_force_single;
      opts.progress_sink = sink;

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
      auto sink = resolve_interlace_sink(iv_progress_mode, iv_quiet, iv_validate);
      if (!sink) return 2;

      svp::builder::InterlaceValidateOptions opts;
      opts.svpi_path = iv_svpi;
      opts.media_path = iv_media;
      opts.ffprobe_path = iv_ffprobe;
      opts.validation_codes_path = iv_codes;
      opts.progress_sink = sink;

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
      auto sink = resolve_interlace_sink(ie_progress_mode, ie_quiet, ie_extract);
      if (!sink) return 2;

      svp::builder::InterlaceExtractOptions opts;
      opts.svp_path = ie_svp;
      opts.out_dir = ie_out_dir;
      opts.ffprobe_path = ie_ffprobe;
      opts.validation_codes_path = ie_codes;
      opts.progress_sink = sink;

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
      auto sink = resolve_interlace_sink(ir_progress_mode, ir_quiet, ir_recombine);
      if (!sink) return 2;

      svp::builder::InterlaceRecombineOptions opts;
      opts.media_path = ir_media;
      opts.svpi_path = ir_svpi;
      opts.output_path = ir_out;
      opts.staging_dir = ir_staging;
      opts.ffprobe_path = ir_ffprobe;
      opts.validation_codes_path = ir_codes;
      opts.progress_sink = sink;

      auto result = svp::builder::interlace_recombine(opts);
      if (!result.success) {
        std::cerr << "interlace recombine failed: " << result.error_message << "\n";
        return 1;
      }
      std::cout << "Recombined SVP: " << result.svp_path.string() << "\n";
      std::cout << "Binding state: " << result.binding_state_label << "\n";
      const int svp_exit = svp::validation::exit_code(result.validation_report);
      std::cout << "SVP validation exit code: " << svp_exit << "\n";
      if (svp_exit != 0) {
        std::cout << "Note: SVP validation reported issues (expected for skeleton/diagnostic packages).\n";
        for (const auto& err : result.validation_report.errors) {
          std::cout << "  " << err.code << ": " << err.message << "\n";
        }
      }
      return 0;
    }

    if (*cb_create_batch) {
      auto visibility = svp::builder::parse_sidecar_visibility(cb_visibility);
      if (!visibility) {
        std::cerr << "invalid sidecar visibility: " << cb_visibility << "\n";
        return 2;
      }

      svp::builder::BatchCreateOptions opts;
      opts.source_dir = cb_source_dir;
      opts.out_dir = cb_out_dir;
      opts.model_cache_dir = cb_model_cache;
      opts.ffprobe_path = cb_ffprobe;
      opts.ffmpeg_path = cb_ffmpeg;
      opts.staging_dir = cb_staging;
      opts.sherpa_lib_path = cb_sherpa_lib;
      opts.recursive = cb_recursive;
      opts.visibility = *visibility;
      opts.no_blake3 = cb_no_blake3;
      opts.replace_mismatched = cb_replace_mismatched;
      opts.core_only_diagnostic = cb_core_only;
      opts.allow_fallback_diarization = cb_allow_fallback;
      opts.force_single_speaker = cb_force_single;
      opts.progress_sink = resolve_interlace_sink(cb_progress_mode, cb_quiet, cb_create_batch);

      auto result = svp::builder::interlace_create_batch(opts);

      if (cb_json) {
        nlohmann::json j;
        j["created"] = result.created_count;
        j["already_valid"] = result.already_valid_count;
        j["skipped"] = result.skipped_count;
        j["binding_mismatch"] = result.mismatch_count;
        j["failed"] = result.failed_count;
        j["replaced"] = result.replaced_count;
        nlohmann::json files = nlohmann::json::array();
        for (const auto& r : result.results) {
          nlohmann::json file;
          file["source"] = r.source_filename;
          file["status"] = std::string(svp::builder::batch_file_status_label(r.status));
          if (!r.error_message.empty()) file["error"] = r.error_message;
          if (!r.blake3_state.empty()) file["blake3_state"] = r.blake3_state;
          files.push_back(std::move(file));
        }
        j["files"] = files;
        std::cout << j.dump(2) << "\n";
      } else {
        std::cout << "Batch create summary:\n";
        std::cout << "  created: " << result.created_count << "\n";
        std::cout << "  already_valid: " << result.already_valid_count << "\n";
        std::cout << "  binding_mismatch: " << result.mismatch_count << "\n";
        std::cout << "  failed: " << result.failed_count << "\n";
        std::cout << "  replaced: " << result.replaced_count << "\n";
        for (const auto& r : result.results) {
          std::cout << "  " << r.source_filename << ": "
                    << svp::builder::batch_file_status_label(r.status) << "\n";
          if (!r.error_message.empty()) {
            std::cout << "    error: " << r.error_message << "\n";
          }
        }
      }
      return result.failed_count > 0 ? 1 : 0;
    }

    if (*sc_scan) {
      svp::builder::ScanOptions opts;
      opts.source_dir = sc_source_dir;
      opts.recursive = sc_recursive;
      opts.ffprobe_path = sc_ffprobe;
      opts.validation_codes_path = sc_codes;

      auto result = svp::builder::interlace_scan(opts);

      if (sc_json) {
        nlohmann::json j;
        j["total_media"] = result.total_media;
        j["total_svpi"] = result.total_svpi;
        j["matched_pairs"] = result.matched_pairs;
        j["verified_pairs"] = result.verified_pairs;
        j["missing_sidecars"] = result.missing_sidecars;
        j["unbound_sidecars"] = result.unbound_sidecars;
        nlohmann::json pairs = nlohmann::json::array();
        for (const auto& p : result.pairs) {
          nlohmann::json pair;
          pair["media"] = p.media_filename;
          pair["svpi_found"] = p.svpi_found;
          pair["binding_verified"] = p.binding_verified;
          if (!p.binding_state_label.empty()) pair["binding_state"] = p.binding_state_label;
          if (!p.svpi_error.empty()) pair["svpi_error"] = p.svpi_error;
          pairs.push_back(std::move(pair));
        }
        j["pairs"] = pairs;
        std::cout << j.dump(2) << "\n";
      } else {
        std::cout << "Scan results:\n";
        std::cout << "  total media: " << result.total_media << "\n";
        std::cout << "  total SVPI: " << result.total_svpi << "\n";
        std::cout << "  matched pairs: " << result.matched_pairs << "\n";
        std::cout << "  verified pairs: " << result.verified_pairs << "\n";
        if (!result.missing_sidecars.empty()) {
          std::cout << "  missing sidecars:\n";
          for (const auto& m : result.missing_sidecars) {
            std::cout << "    " << m << "\n";
          }
        }
        if (!result.unbound_sidecars.empty()) {
          std::cout << "  unbound sidecars:\n";
          for (const auto& u : result.unbound_sidecars) {
            std::cout << "    " << u << "\n";
          }
        }
        for (const auto& p : result.pairs) {
          std::cout << "  " << p.media_filename << " -> "
                    << (p.svpi_found ? "paired" : "no sidecar")
                    << (p.binding_verified ? " (verified)" : "")
                    << "\n";
        }
      }
      return 0;
    }

    if (*vb_validate_batch) {
      auto sink = resolve_interlace_sink(vb_progress_mode, vb_quiet, vb_validate_batch);
      if (!sink) return 2;

      svp::builder::BatchValidateOptions opts;
      opts.source_dir = vb_source_dir;
      opts.recursive = vb_recursive;
      opts.ffprobe_path = vb_ffprobe;
      opts.validation_codes_path = vb_codes;
      opts.progress_sink = sink;

      auto result = svp::builder::interlace_validate_batch(opts);

      if (vb_json) {
        nlohmann::json j;
        j["valid_bound"] = result.valid_bound_count;
        j["valid_unbound"] = result.valid_unbound_count;
        j["binding_mismatch"] = result.mismatch_count;
        j["invalid_structure"] = result.invalid_structure_count;
        j["failed"] = result.failed_count;
        nlohmann::json files = nlohmann::json::array();
        for (const auto& r : result.results) {
          nlohmann::json file;
          file["svpi"] = r.svpi_filename;
          file["state"] = std::string(svp::builder::batch_validation_state_label(r.state));
          if (!r.media_filename.empty()) file["media"] = r.media_filename;
          if (!r.errors.empty()) file["errors"] = r.errors;
          files.push_back(std::move(file));
        }
        j["files"] = files;
        std::cout << j.dump(2) << "\n";
      } else {
        std::cout << "Batch validate summary:\n";
        std::cout << "  valid_bound: " << result.valid_bound_count << "\n";
        std::cout << "  valid_unbound: " << result.valid_unbound_count << "\n";
        std::cout << "  binding_mismatch: " << result.mismatch_count << "\n";
        std::cout << "  invalid_structure: " << result.invalid_structure_count << "\n";
        std::cout << "  failed: " << result.failed_count << "\n";
        for (const auto& r : result.results) {
          std::cout << "  " << r.svpi_filename << ": "
                    << svp::builder::batch_validation_state_label(r.state) << "\n";
          for (const auto& err : r.errors) {
            std::cout << "    " << err << "\n";
          }
        }
      }
      return (result.mismatch_count + result.invalid_structure_count + result.failed_count) > 0 ? 1 : 0;
    }

    if (*ci_complete) {
      auto sink = resolve_interlace_sink(ci_progress_mode, ci_quiet, ci_complete);
      if (!sink) return 2;

      svp::builder::CompleteIdentityOptions opts;
      opts.svpi_path = ci_svpi;
      opts.media_path = ci_media;
      opts.ffprobe_path = ci_ffprobe;
      opts.validation_codes_path = ci_codes;
      opts.progress_sink = sink;

      auto result = svp::builder::interlace_complete_identity(opts);
      if (!result.success) {
        std::cerr << "complete-identity failed: " << result.error_message << "\n";
        return 1;
      }
      std::cout << "Identity completion for " << ci_svpi << "\n";
      std::cout << "  previous state: " << result.previous_state << "\n";
      std::cout << "  new state: " << result.new_state << "\n";
      if (!result.blake3_hash.empty()) {
        std::cout << "  blake3 hash: " << result.blake3_hash << "\n";
      }
      std::cout << "  rebuilt: " << (result.rebuilt ? "yes" : "no") << "\n";
      return 0;
    }

    if (*cib_complete_batch) {
      auto sink = resolve_interlace_sink(cib_progress_mode, cib_quiet, cib_complete_batch);
      if (!sink) return 2;

      svp::builder::CompleteIdentityBatchOptions opts;
      opts.source_dir = cib_source_dir;
      opts.recursive = cib_recursive;
      opts.ffprobe_path = cib_ffprobe;
      opts.validation_codes_path = cib_codes;
      opts.progress_sink = sink;

      auto result = svp::builder::interlace_complete_identity_batch(opts);

      if (cib_json) {
        nlohmann::json j;
        j["completed"] = result.completed_count;
        j["already_present"] = result.already_present_count;
        j["failed"] = result.failed_count;
        std::cout << j.dump(2) << "\n";
      } else {
        std::cout << "Complete-identity batch summary:\n";
        std::cout << "  completed: " << result.completed_count << "\n";
        std::cout << "  already_present: " << result.already_present_count << "\n";
        std::cout << "  failed: " << result.failed_count << "\n";
        for (size_t i = 0; i < result.results.size(); ++i) {
          std::cout << "  " << result.svpi_filenames[i] << ": "
                    << (result.results[i].success ? "ok" : "failed");
          if (!result.results[i].error_message.empty()) {
            std::cout << " - " << result.results[i].error_message;
          }
          std::cout << "\n";
        }
      }
      return result.failed_count > 0 ? 1 : 0;
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

      auto progress_opt = build->get_option("--progress");
      const bool progress_explicitly_set =
          progress_opt && progress_opt->count() > 0;

      std::optional<svp::builder::ProgressMode> resolved_mode;
      if (build_quiet) {
        if (progress_explicitly_set && build_progress_mode == "json") {
          resolved_mode = svp::builder::ProgressMode::json;
        } else {
          resolved_mode = svp::builder::ProgressMode::none;
        }
      } else {
        resolved_mode = svp::builder::parse_progress_mode(build_progress_mode);
      }

      if (!resolved_mode) {
        std::cerr << "svp-builder: invalid --progress value: "
                  << build_progress_mode << "\n";
        return 2;
      }

      const bool stderr_is_tty = isatty(fileno(stderr)) != 0;
      auto progress_sink = svp::builder::make_progress_sink(
          *resolved_mode, std::cerr, stderr_is_tty);

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
      options.progress_sink = progress_sink;

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
