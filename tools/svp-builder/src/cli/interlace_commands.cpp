#include "cli_context.hpp"
#include "cli_elapsed.hpp"

#include "svp/builder/interlace.hpp"
#include "svp/builder/interlace_batch.hpp"
#include "svp/validation/report_json.hpp"

#include <iostream>
#include <chrono>

int run_interlace_command(const InterlaceCliOptions& opts) {
  // interlace create
  if (*opts.ic_create_sub) {
    auto sink = resolve_cli_progress_sink(opts.ic_progress_mode, opts.ic_quiet, opts.ic_create_sub);
    if (!sink) return 2;

    svp::builder::InterlaceCreateOptions ic_opts;
    ic_opts.source_path = opts.ic_source;
    ic_opts.output_path = opts.ic_out;
    ic_opts.staging_dir = opts.ic_staging;
    ic_opts.model_cache_dir = opts.ic_model_cache;
    ic_opts.ffprobe_path = opts.ic_ffprobe;
    ic_opts.ffmpeg_path = opts.ic_ffmpeg;
    ic_opts.probe_json_path = opts.ic_probe_json;
    ic_opts.sherpa_lib_path = opts.ic_sherpa_lib;
    ic_opts.performance = opts.ic_performance;
    ic_opts.compute_full_blake3 = !opts.ic_no_blake3;
    ic_opts.core_only_diagnostic = opts.ic_core_only;
    ic_opts.allow_fallback_diarization = opts.ic_allow_fallback;
    ic_opts.force_single_speaker = opts.ic_force_single;
    ic_opts.serial_pipeline = opts.ic_serial_pipeline;
    ic_opts.progress_sink = sink;

    const auto started_at = std::chrono::steady_clock::now();
    auto result = svp::builder::interlace_create(ic_opts);
    if (!result.success) {
      std::cerr << "interlace create failed: " << result.error_message << "\n";
      return 1;
    }
    std::cout << "SVPI created: " << result.svpi_path.string() << "\n";
    std::cout << "BLAKE3 state: " << result.blake3_state << "\n";
    std::cout << "Validation status: " << result.binding_state << "\n";
    std::cout << "SVPI created in "
              << format_elapsed_duration(std::chrono::steady_clock::now() -
                                         started_at)
              << "\n";
    return 0;
  }

  // interlace validate
  if (*opts.iv_validate_sub) {
    auto sink = resolve_cli_progress_sink(opts.iv_progress_mode, opts.iv_quiet, opts.iv_validate_sub);
    if (!sink) return 2;

    svp::builder::InterlaceValidateOptions iv_opts;
    iv_opts.svpi_path = opts.iv_svpi;
    iv_opts.media_path = opts.iv_media;
    iv_opts.ffprobe_path = opts.iv_ffprobe;
    iv_opts.validation_codes_path = opts.iv_codes;
    iv_opts.progress_sink = sink;

    auto result = svp::builder::interlace_validate(iv_opts);

    render_validate_output(result, opts.iv_svpi, opts.iv_json);
    return result.structure_valid && (!result.binding_attempted || result.binding_verified) ? 0 : 1;
  }

  // interlace inspect
  if (*opts.ii_inspect_sub) {
    svp::builder::InterlaceInspectOptions ii_opts;
    ii_opts.svpi_path = opts.ii_svpi;

    auto result = svp::builder::interlace_inspect(ii_opts);
    if (!result.success) {
      std::cerr << "interlace inspect failed: " << result.error_message << "\n";
      return 1;
    }

    render_inspect_output(result, opts.ii_json);
    return 0;
  }

  // interlace extract
  if (*opts.ie_extract_sub) {
    auto sink = resolve_cli_progress_sink(opts.ie_progress_mode, opts.ie_quiet, opts.ie_extract_sub);
    if (!sink) return 2;

    svp::builder::InterlaceExtractOptions ie_opts;
    ie_opts.svp_path = opts.ie_svp;
    ie_opts.out_dir = opts.ie_out_dir;
    ie_opts.ffprobe_path = opts.ie_ffprobe;
    ie_opts.validation_codes_path = opts.ie_codes;
    ie_opts.progress_sink = sink;

    auto result = svp::builder::interlace_extract(ie_opts);
    if (!result.success) {
      std::cerr << "interlace extract failed: " << result.error_message << "\n";
      return 1;
    }
    std::cout << "Extracted media: " << result.extracted_media_path.string() << "\n";
    std::cout << "Extracted SVPI: " << result.extracted_svpi_path.string() << "\n";
    return 0;
  }

  // interlace recombine
  if (*opts.ir_recombine_sub) {
    auto sink = resolve_cli_progress_sink(opts.ir_progress_mode, opts.ir_quiet, opts.ir_recombine_sub);
    if (!sink) return 2;

    svp::builder::InterlaceRecombineOptions ir_opts;
    ir_opts.media_path = opts.ir_media;
    ir_opts.svpi_path = opts.ir_svpi;
    ir_opts.output_path = opts.ir_out;
    ir_opts.staging_dir = opts.ir_staging;
    ir_opts.ffprobe_path = opts.ir_ffprobe;
    ir_opts.validation_codes_path = opts.ir_codes;
    ir_opts.progress_sink = sink;

    const auto started_at = std::chrono::steady_clock::now();
    auto result = svp::builder::interlace_recombine(ir_opts);
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
    std::cout << "SVP created in "
              << format_elapsed_duration(std::chrono::steady_clock::now() -
                                         started_at)
              << "\n";
    return 0;
  }

  // interlace create-batch
  if (*opts.cb_create_batch_sub) {
    auto output_format = svp::builder::parse_batch_output_format(
        opts.cb_output_format);
    if (!output_format) {
      std::cerr << "invalid batch output format: "
                << opts.cb_output_format << "\n";
      return 2;
    }
    auto visibility = svp::builder::parse_sidecar_visibility(opts.cb_visibility);
    if (!visibility) {
      std::cerr << "invalid sidecar visibility: " << opts.cb_visibility << "\n";
      return 2;
    }
    if (*output_format == svp::builder::BatchOutputFormat::embedded_svpi) {
      const bool has_output_directory =
          !opts.cb_out_dir.empty() && opts.cb_out_dir != "same-as-source";
      if (!has_output_directory && !opts.cb_overwrite) {
        std::cerr << "--output-format embedded-svpi requires either --out-dir or explicit --overwrite\n";
        return 2;
      }
      if (has_output_directory && opts.cb_overwrite) {
        std::cerr << "--overwrite performs in-place replacement and cannot be combined with --out-dir\n";
        return 2;
      }
      if (*visibility != svp::builder::SidecarVisibility::visible) {
        std::cerr << "--sidecar-visibility applies only to --output-format svpi\n";
        return 2;
      }
      if (opts.cb_no_blake3) {
        std::cerr << "--no-blake3 cannot be used with --output-format embedded-svpi\n";
        return 2;
      }
    } else if (opts.cb_overwrite) {
      std::cerr << "--overwrite is supported only with --output-format embedded-svpi\n";
      return 2;
    }

    svp::builder::BatchCreateOptions cb_opts;
    cb_opts.source_dir = opts.cb_source_dir;
    cb_opts.out_dir = opts.cb_out_dir;
    cb_opts.model_cache_dir = opts.cb_model_cache;
    cb_opts.ffprobe_path = opts.cb_ffprobe;
    cb_opts.ffmpeg_path = opts.cb_ffmpeg;
    cb_opts.staging_dir = opts.cb_staging;
    cb_opts.sherpa_lib_path = opts.cb_sherpa_lib;
    cb_opts.performance = opts.cb_performance;
    cb_opts.jobs = opts.cb_jobs;
    cb_opts.recursive = opts.cb_recursive;
    cb_opts.output_format = *output_format;
    cb_opts.visibility = *visibility;
    cb_opts.no_blake3 = opts.cb_no_blake3;
    cb_opts.replace_mismatched = opts.cb_replace_mismatched;
    cb_opts.overwrite_sources = opts.cb_overwrite;
    cb_opts.core_only_diagnostic = opts.cb_core_only;
    cb_opts.allow_fallback_diarization = opts.cb_allow_fallback;
    cb_opts.force_single_speaker = opts.cb_force_single;
    cb_opts.serial_pipeline = opts.cb_serial_pipeline;
    cb_opts.progress_sink = resolve_cli_progress_sink(opts.cb_progress_mode, opts.cb_quiet, opts.cb_create_batch_sub);

    auto result = svp::builder::interlace_create_batch(cb_opts);

    render_create_batch_output(result, opts.cb_json);
    return result.failed_count > 0 ? 1 : 0;
  }

  // interlace scan
  if (*opts.sc_scan_sub) {
    svp::builder::ScanOptions sc_opts;
    sc_opts.source_dir = opts.sc_source_dir;
    sc_opts.recursive = opts.sc_recursive;
    sc_opts.ffprobe_path = opts.sc_ffprobe;
    sc_opts.validation_codes_path = opts.sc_codes;

    auto result = svp::builder::interlace_scan(sc_opts);

    render_scan_output(result, opts.sc_json);
    return 0;
  }

  // interlace validate-batch
  if (*opts.vb_validate_batch_sub) {
    auto sink = resolve_cli_progress_sink(opts.vb_progress_mode, opts.vb_quiet, opts.vb_validate_batch_sub);
    if (!sink) return 2;

    svp::builder::BatchValidateOptions vb_opts;
    vb_opts.source_dir = opts.vb_source_dir;
    vb_opts.recursive = opts.vb_recursive;
    vb_opts.ffprobe_path = opts.vb_ffprobe;
    vb_opts.validation_codes_path = opts.vb_codes;
    vb_opts.progress_sink = sink;

    auto result = svp::builder::interlace_validate_batch(vb_opts);

    render_validate_batch_output(result, opts.vb_json);
    return (result.mismatch_count + result.invalid_structure_count + result.failed_count) > 0 ? 1 : 0;
  }

  // interlace complete-identity
  if (*opts.ci_complete_sub) {
    auto sink = resolve_cli_progress_sink(opts.ci_progress_mode, opts.ci_quiet, opts.ci_complete_sub);
    if (!sink) return 2;

    svp::builder::CompleteIdentityOptions ci_opts;
    ci_opts.svpi_path = opts.ci_svpi;
    ci_opts.media_path = opts.ci_media;
    ci_opts.ffprobe_path = opts.ci_ffprobe;
    ci_opts.validation_codes_path = opts.ci_codes;
    ci_opts.progress_sink = sink;

    auto result = svp::builder::interlace_complete_identity(ci_opts);
    if (!result.success) {
      std::cerr << "complete-identity failed: " << result.error_message << "\n";
      return 1;
    }
    std::cout << "Identity completion for " << opts.ci_svpi << "\n";
    std::cout << "  previous state: " << result.previous_state << "\n";
    std::cout << "  new state: " << result.new_state << "\n";
    if (!result.blake3_hash.empty()) {
      std::cout << "  blake3 hash: " << result.blake3_hash << "\n";
    }
    std::cout << "  rebuilt: " << (result.rebuilt ? "yes" : "no") << "\n";
    return 0;
  }

  // interlace complete-identity-batch
  if (*opts.cib_complete_batch_sub) {
    auto sink = resolve_cli_progress_sink(opts.cib_progress_mode, opts.cib_quiet, opts.cib_complete_batch_sub);
    if (!sink) return 2;

    svp::builder::CompleteIdentityBatchOptions cib_opts;
    cib_opts.source_dir = opts.cib_source_dir;
    cib_opts.recursive = opts.cib_recursive;
    cib_opts.ffprobe_path = opts.cib_ffprobe;
    cib_opts.validation_codes_path = opts.cib_codes;
    cib_opts.progress_sink = sink;

    auto result = svp::builder::interlace_complete_identity_batch(cib_opts);

    render_complete_identity_batch_output(result, opts.cib_json);
    return result.failed_count > 0 ? 1 : 0;
  }

  return 0;
}
