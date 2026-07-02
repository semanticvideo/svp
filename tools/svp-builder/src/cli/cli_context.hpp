#pragma once

#include <CLI/CLI.hpp>

#include <string>

namespace svp::builder {
class BuildProgressSink;
struct InterlaceValidateResult;
struct InterlaceInspectResult;
struct BatchCreateResult;
struct ScanResult;
struct BatchValidateResult;
struct CompleteIdentityBatchResult;
}

struct ProbeCliOptions {
  std::string source_path;
  std::string probe_json_path;
  std::string ffprobe_path = "ffprobe";
  bool json_output = false;
};

struct DiarizeCliOptions {
  std::string wav_path;
  std::string model_dir;
  std::string sherpa_lib_path;
  std::string segments_jsonl_path;
};

struct BuildCliOptions {
  std::string source_path;
  std::string probe_json_path;
  std::string ffprobe_path = "ffprobe";
  std::string ffmpeg_path = "ffmpeg";
  std::string output_path;
  std::string staging_dir;
  std::string model_cache_dir;
  std::string stop_after = "media-ingest";
  std::string sherpa_lib_path;
  bool allow_fallback_diarization = false;
  bool force_single_speaker = false;
  std::string progress_mode = "auto";
  bool quiet = false;
  bool verbose = false;
};

struct InterlaceCliOptions {
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

  // interlace validate
  std::string iv_svpi;
  std::string iv_media;
  std::string iv_ffprobe = "ffprobe";
  std::string iv_codes = "spec/registries/validation-codes.json";
  bool iv_json = false;
  std::string iv_progress_mode = "auto";
  bool iv_quiet = false;

  // interlace inspect
  std::string ii_svpi;
  bool ii_json = false;

  // interlace extract
  std::string ie_svp;
  std::string ie_out_dir;
  std::string ie_ffprobe = "ffprobe";
  std::string ie_codes = "spec/registries/validation-codes.json";
  std::string ie_progress_mode = "auto";
  bool ie_quiet = false;

  // interlace recombine
  std::string ir_media;
  std::string ir_svpi;
  std::string ir_out;
  std::string ir_staging;
  std::string ir_ffprobe = "ffprobe";
  std::string ir_codes = "spec/registries/validation-codes.json";
  std::string ir_progress_mode = "auto";
  bool ir_quiet = false;

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

  // interlace scan
  std::string sc_source_dir;
  bool sc_recursive = false;
  bool sc_json = false;
  std::string sc_ffprobe = "ffprobe";
  std::string sc_codes = "spec/registries/validation-codes.json";

  // interlace validate-batch
  std::string vb_source_dir;
  bool vb_recursive = false;
  bool vb_json = false;
  std::string vb_ffprobe = "ffprobe";
  std::string vb_codes = "spec/registries/validation-codes.json";
  std::string vb_progress_mode = "auto";
  bool vb_quiet = false;

  // interlace complete-identity
  std::string ci_svpi;
  std::string ci_media;
  std::string ci_ffprobe = "ffprobe";
  std::string ci_codes = "spec/registries/validation-codes.json";
  std::string ci_progress_mode = "auto";
  bool ci_quiet = false;

  // interlace complete-identity-batch
  std::string cib_source_dir;
  bool cib_recursive = false;
  bool cib_json = false;
  std::string cib_ffprobe = "ffprobe";
  std::string cib_codes = "spec/registries/validation-codes.json";
  std::string cib_progress_mode = "auto";
  bool cib_quiet = false;

  // subcommand pointers (set by register_cli)
  CLI::App* ic_create_sub = nullptr;
  CLI::App* iv_validate_sub = nullptr;
  CLI::App* ii_inspect_sub = nullptr;
  CLI::App* ie_extract_sub = nullptr;
  CLI::App* ir_recombine_sub = nullptr;
  CLI::App* cb_create_batch_sub = nullptr;
  CLI::App* sc_scan_sub = nullptr;
  CLI::App* vb_validate_batch_sub = nullptr;
  CLI::App* ci_complete_sub = nullptr;
  CLI::App* cib_complete_batch_sub = nullptr;
};

struct CliContext {
  ProbeCliOptions probe_opts;
  BuildCliOptions build_opts;
  InterlaceCliOptions interlace_opts;

  DiarizeCliOptions diarize_opts;

  CLI::App* probe_subcommand = nullptr;
  CLI::App* build_subcommand = nullptr;
  CLI::App* diarize_subcommand = nullptr;
  CLI::App* interlace_subcommand = nullptr;

  CLI::App* ic_create = nullptr;
  CLI::App* iv_validate = nullptr;
  CLI::App* ii_inspect = nullptr;
  CLI::App* ie_extract = nullptr;
  CLI::App* ir_recombine = nullptr;
  CLI::App* cb_create_batch = nullptr;
  CLI::App* sc_scan = nullptr;
  CLI::App* vb_validate_batch = nullptr;
  CLI::App* ci_complete = nullptr;
  CLI::App* cib_complete_batch = nullptr;
};

void register_cli(CLI::App& app, CliContext& context);

std::shared_ptr<svp::builder::BuildProgressSink> resolve_cli_progress_sink(
    const std::string& mode,
    bool quiet,
    CLI::App* subcommand);

int run_probe_command(const ProbeCliOptions& options);

int run_build_command(const BuildCliOptions& options, CLI::App* build_subcommand);

int run_diarize_command(const DiarizeCliOptions& options);

int run_interlace_command(const InterlaceCliOptions& options);

int run_selected_command(const CliContext& context);

void render_validate_output(const svp::builder::InterlaceValidateResult& result,
                            const std::string& svpi_path, bool json);
void render_inspect_output(const svp::builder::InterlaceInspectResult& result,
                           bool json);
void render_create_batch_output(const svp::builder::BatchCreateResult& result,
                                bool json);
void render_scan_output(const svp::builder::ScanResult& result, bool json);
void render_validate_batch_output(const svp::builder::BatchValidateResult& result,
                                  bool json);
void render_complete_identity_batch_output(
    const svp::builder::CompleteIdentityBatchResult& result, bool json);
