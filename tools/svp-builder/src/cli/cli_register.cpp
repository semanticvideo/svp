#include "cli_context.hpp"

void register_cli(CLI::App& app, CliContext& context) {
  auto& probe_opts = context.probe_opts;
  auto& build_opts = context.build_opts;
  auto& opts = context.interlace_opts;

  // --- probe subcommand ---
  auto* probe = app.add_subcommand(
      "probe", "Read deterministic media probe metadata and compute SVP timing data");
  probe->add_option("source", probe_opts.source_path, "Source media path")->required();
  probe->add_option("--probe-json", probe_opts.probe_json_path,
                    "Precomputed media probe JSON; skips running ffprobe");
  probe->add_option("--ffprobe", probe_opts.ffprobe_path, "ffprobe executable path");
  probe->add_flag("--json", probe_opts.json_output, "Emit JSON");
  context.probe_subcommand = probe;

  // --- build subcommand ---
  auto* build = app.add_subcommand(
      "build", "Write an honest builder foundation JSON artifact");
  build->add_option("source", build_opts.source_path, "Source media path")->required();
  build->add_option("--probe-json", build_opts.probe_json_path,
                    "Precomputed media probe JSON; skips running ffprobe");
  build->add_option("--ffprobe", build_opts.ffprobe_path, "ffprobe executable path");
  build->add_option("--ffmpeg", build_opts.ffmpeg_path, "ffmpeg executable path");
  build->add_option("--out", build_opts.output_path,
                    "Output path for the builder foundation JSON")
      ->required();
  build->add_option("--staging-dir", build_opts.staging_dir,
                    "Directory for staged builder outputs");
  build->add_option("--model-cache", build_opts.model_cache_dir,
                    "Path to SVP model cache directory containing model bundles");
  build->add_option("--stop-after", build_opts.stop_after,
                    "Supported foundation stages: media-ingest, audio, vision-plan, "
                    "foundation-color, foundation-ocr, package-skeleton");
  build->add_option("--sherpa-lib", build_opts.sherpa_lib_path,
                    "Explicit path to libsherpa-onnx-c-api.dylib for diarization");
  build->add_flag("--allow-fallback-diarization", build_opts.allow_fallback_diarization,
                  "Proceed without diarization if sherpa-onnx is not available. "
                  "Speaker data will be fabricated fallback, not real. NOT RECOMMENDED.");
  build->add_flag("--force-single-speaker", build_opts.force_single_speaker,
                  "Skip Sherpa diarization entirely and emit one speaker segment. "
                  "Use when you know the clip contains only one speaker.");
  build->add_option("--progress", build_opts.progress_mode,
                    "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  build->add_flag("--quiet", build_opts.quiet,
                  "Suppress progress output; print only final success/failure");
  build->add_flag("--verbose", build_opts.verbose,
                  "Include detailed diagnostics and full validation findings");
  context.build_subcommand = build;

  // --- diarize subcommand (diagnostic) ---
  auto* diarize = app.add_subcommand(
      "diarize", "Run Sherpa diarization on a WAV file without ASR (diagnostic)");
  diarize->add_option("wav", context.diarize_opts.wav_path,
                      "Path to 16kHz mono PCM WAV")->required();
  diarize->add_option("--model-dir", context.diarize_opts.model_dir,
                      "Sherpa diarization model directory")->required();
  diarize->add_option("--sherpa-lib", context.diarize_opts.sherpa_lib_path,
                      "Explicit path to libsherpa-onnx-c-api.dylib");
  diarize->add_option("--segments-jsonl", context.diarize_opts.segments_jsonl_path,
                      "Write diarization segments as JSONL for diagnostics");
  context.diarize_subcommand = diarize;

  // --- interlace subcommand ---
  auto* interlace = app.add_subcommand(
      "interlace", "SVPI sidecar operations: create, validate, inspect, extract, recombine");
  context.interlace_subcommand = interlace;

  // interlace create
  auto* ic_create = interlace->add_subcommand(
      "create", "Create a .svpi sidecar from source media");
  ic_create->add_option("source", opts.ic_source, "Source media path")->required();
  ic_create->add_option("--out", opts.ic_out, "Output .svpi path")->required();
  ic_create->add_option("--staging-dir", opts.ic_staging, "Staging directory");
  ic_create->add_option("--model-cache", opts.ic_model_cache, "Model cache directory");
  ic_create->add_option("--ffprobe", opts.ic_ffprobe, "ffprobe executable path");
  ic_create->add_option("--ffmpeg", opts.ic_ffmpeg, "ffmpeg executable path");
  ic_create->add_option("--probe-json", opts.ic_probe_json, "Precomputed probe JSON");
  ic_create->add_option("--sherpa-lib", opts.ic_sherpa_lib, "Path to sherpa-onnx shared library");
  ic_create->add_flag("--no-blake3", opts.ic_no_blake3, "Skip full-file BLAKE3 computation");
  ic_create->add_flag("--core-only-diagnostic", opts.ic_core_only,
      "Emit core-only SVPI without running semantic pipeline (diagnostic mode)");
  ic_create->add_flag("--allow-fallback-diarization", opts.ic_allow_fallback,
      "Allow fallback diarization when sherpa-onnx is unavailable");
  ic_create->add_flag("--force-single-speaker", opts.ic_force_single,
      "Force single-speaker diarization");
  ic_create->add_option("--progress", opts.ic_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ic_create->add_flag("--quiet", opts.ic_quiet,
      "Suppress progress output; print only final success/failure");
  context.ic_create = ic_create;
  opts.ic_create_sub = ic_create;

  // interlace validate
  auto* iv_validate = interlace->add_subcommand(
      "validate", "Validate a .svpi sidecar (structure and optional binding)");
  iv_validate->add_option("svpi", opts.iv_svpi, "SVPI file path")->required();
  iv_validate->add_option("--media", opts.iv_media, "Candidate media file for binding verification");
  iv_validate->add_option("--ffprobe", opts.iv_ffprobe, "ffprobe executable path");
  iv_validate->add_option("--validation-codes", opts.iv_codes, "Validation codes registry path");
  iv_validate->add_flag("--json", opts.iv_json, "Emit JSON output");
  iv_validate->add_option("--progress", opts.iv_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  iv_validate->add_flag("--quiet", opts.iv_quiet,
      "Suppress progress output; print only final success/failure");
  context.iv_validate = iv_validate;
  opts.iv_validate_sub = iv_validate;

  // interlace inspect
  auto* ii_inspect = interlace->add_subcommand(
      "inspect", "Inspect a .svpi sidecar");
  ii_inspect->add_option("svpi", opts.ii_svpi, "SVPI file path")->required();
  ii_inspect->add_flag("--json", opts.ii_json, "Emit JSON output");
  context.ii_inspect = ii_inspect;
  opts.ii_inspect_sub = ii_inspect;

  // interlace extract
  auto* ie_extract = interlace->add_subcommand(
      "extract", "Extract source media and .svpi from a .svp package");
  ie_extract->add_option("svp", opts.ie_svp, "SVP package path")->required();
  ie_extract->add_option("--out-dir", opts.ie_out_dir, "Output directory")->required();
  ie_extract->add_option("--ffprobe", opts.ie_ffprobe, "ffprobe executable path");
  ie_extract->add_option("--validation-codes", opts.ie_codes, "Validation codes registry path");
  ie_extract->add_option("--progress", opts.ie_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ie_extract->add_flag("--quiet", opts.ie_quiet,
      "Suppress progress output; print only final success/failure");
  context.ie_extract = ie_extract;
  opts.ie_extract_sub = ie_extract;

  // interlace recombine
  auto* ir_recombine = interlace->add_subcommand(
      "recombine", "Recombine source media + .svpi into a .svp package");
  ir_recombine->add_option("media", opts.ir_media, "Source media file")->required();
  ir_recombine->add_option("svpi", opts.ir_svpi, "SVPI sidecar file")->required();
  ir_recombine->add_option("--out", opts.ir_out, "Output .svp path")->required();
  ir_recombine->add_option("--staging-dir", opts.ir_staging, "Staging directory");
  ir_recombine->add_option("--ffprobe", opts.ir_ffprobe, "ffprobe executable path");
  ir_recombine->add_option("--validation-codes", opts.ir_codes, "Validation codes registry path");
  ir_recombine->add_option("--progress", opts.ir_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ir_recombine->add_flag("--quiet", opts.ir_quiet,
      "Suppress progress output; print only final success/failure");
  context.ir_recombine = ir_recombine;
  opts.ir_recombine_sub = ir_recombine;

  // interlace create-batch
  auto* cb_create_batch = interlace->add_subcommand(
      "create-batch", "Create .svpi sidecars for all supported videos in a directory");
  cb_create_batch->add_option("source-dir", opts.cb_source_dir, "Directory containing source videos")->required();
  cb_create_batch->add_option("--out-dir", opts.cb_out_dir, "Output directory (default: same as source)");
  cb_create_batch->add_option("--model-cache", opts.cb_model_cache, "Model cache directory");
  cb_create_batch->add_option("--ffprobe", opts.cb_ffprobe, "ffprobe executable path");
  cb_create_batch->add_option("--ffmpeg", opts.cb_ffmpeg, "ffmpeg executable path");
  cb_create_batch->add_option("--staging-dir", opts.cb_staging, "Staging directory");
  cb_create_batch->add_option("--sherpa-lib", opts.cb_sherpa_lib, "Path to sherpa-onnx shared library");
  cb_create_batch->add_option("--sidecar-visibility", opts.cb_visibility,
      "Sidecar naming: visible, hidden, managed-dir")
      ->check(CLI::IsMember({"visible", "hidden", "managed-dir"}));
  cb_create_batch->add_flag("--recursive", opts.cb_recursive, "Search subdirectories recursively");
  cb_create_batch->add_flag("--no-blake3", opts.cb_no_blake3, "Skip full-file BLAKE3 computation");
  cb_create_batch->add_flag("--replace-mismatched", opts.cb_replace_mismatched,
      "Replace existing sidecars that fail binding verification");
  cb_create_batch->add_flag("--json", opts.cb_json, "Emit JSON summary report");
  cb_create_batch->add_flag("--core-only-diagnostic", opts.cb_core_only,
      "Emit core-only SVPI without running semantic pipeline (diagnostic mode)");
  cb_create_batch->add_flag("--allow-fallback-diarization", opts.cb_allow_fallback,
      "Allow fallback diarization when sherpa-onnx is unavailable");
  cb_create_batch->add_flag("--force-single-speaker", opts.cb_force_single,
      "Force single-speaker diarization");
  cb_create_batch->add_option("--progress", opts.cb_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  cb_create_batch->add_flag("--quiet", opts.cb_quiet,
      "Suppress progress output; print only final success/failure");
  context.cb_create_batch = cb_create_batch;
  opts.cb_create_batch_sub = cb_create_batch;

  // interlace scan
  auto* sc_scan = interlace->add_subcommand(
      "scan", "Discover media/SVPI pairs in a directory");
  sc_scan->add_option("source-dir", opts.sc_source_dir, "Directory to scan")->required();
  sc_scan->add_flag("--recursive", opts.sc_recursive, "Search subdirectories recursively");
  sc_scan->add_flag("--json", opts.sc_json, "Emit JSON output");
  sc_scan->add_option("--ffprobe", opts.sc_ffprobe, "ffprobe executable path");
  sc_scan->add_option("--validation-codes", opts.sc_codes, "Validation codes registry path");
  context.sc_scan = sc_scan;
  opts.sc_scan_sub = sc_scan;

  // interlace validate-batch
  auto* vb_validate_batch = interlace->add_subcommand(
      "validate-batch", "Validate all .svpi files in a directory");
  vb_validate_batch->add_option("source-dir", opts.vb_source_dir, "Directory to validate")->required();
  vb_validate_batch->add_flag("--recursive", opts.vb_recursive, "Search subdirectories recursively");
  vb_validate_batch->add_flag("--json", opts.vb_json, "Emit JSON output");
  vb_validate_batch->add_option("--ffprobe", opts.vb_ffprobe, "ffprobe executable path");
  vb_validate_batch->add_option("--validation-codes", opts.vb_codes, "Validation codes registry path");
  vb_validate_batch->add_option("--progress", opts.vb_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  vb_validate_batch->add_flag("--quiet", opts.vb_quiet,
      "Suppress progress output; print only final success/failure");
  context.vb_validate_batch = vb_validate_batch;
  opts.vb_validate_batch_sub = vb_validate_batch;

  // interlace complete-identity
  auto* ci_complete = interlace->add_subcommand(
      "complete-identity", "Complete pending full-file BLAKE3 identity for an SVPI");
  ci_complete->add_option("svpi", opts.ci_svpi, "SVPI file path")->required();
  ci_complete->add_option("--media", opts.ci_media, "Source media file")->required();
  ci_complete->add_option("--ffprobe", opts.ci_ffprobe, "ffprobe executable path");
  ci_complete->add_option("--validation-codes", opts.ci_codes, "Validation codes registry path");
  ci_complete->add_option("--progress", opts.ci_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  ci_complete->add_flag("--quiet", opts.ci_quiet,
      "Suppress progress output; print only final success/failure");
  context.ci_complete = ci_complete;
  opts.ci_complete_sub = ci_complete;

  // interlace complete-identity-batch
  auto* cib_complete_batch = interlace->add_subcommand(
      "complete-identity-batch", "Complete pending BLAKE3 identity for all SVPI files in a directory");
  cib_complete_batch->add_option("source-dir", opts.cib_source_dir, "Directory containing SVPI files")->required();
  cib_complete_batch->add_flag("--recursive", opts.cib_recursive, "Search subdirectories recursively");
  cib_complete_batch->add_flag("--json", opts.cib_json, "Emit JSON output");
  cib_complete_batch->add_option("--ffprobe", opts.cib_ffprobe, "ffprobe executable path");
  cib_complete_batch->add_option("--validation-codes", opts.cib_codes, "Validation codes registry path");
  cib_complete_batch->add_option("--progress", opts.cib_progress_mode,
      "Progress output mode: auto, plain, json, none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));
  cib_complete_batch->add_flag("--quiet", opts.cib_quiet,
      "Suppress progress output; print only final success/failure");
  context.cib_complete_batch = cib_complete_batch;
  opts.cib_complete_batch_sub = cib_complete_batch;
}
