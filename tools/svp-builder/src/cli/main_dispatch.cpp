#include "cli_context.hpp"

int run_selected_command(const CliContext& context) {
  // interlace first, if any interlace subcommand was selected
  if (*context.interlace_opts.ic_create_sub ||
      *context.interlace_opts.iv_validate_sub ||
      *context.interlace_opts.ii_inspect_sub ||
      *context.interlace_opts.ie_extract_sub ||
      *context.interlace_opts.ir_recombine_sub ||
      *context.interlace_opts.em_embed_sub ||
      *context.interlace_opts.ee_extract_sub ||
      *context.interlace_opts.se_strip_sub ||
      *context.interlace_opts.cb_create_batch_sub ||
      *context.interlace_opts.sc_scan_sub ||
      *context.interlace_opts.vb_validate_batch_sub ||
      *context.interlace_opts.ci_complete_sub ||
      *context.interlace_opts.cib_complete_batch_sub) {
    return run_interlace_command(context.interlace_opts);
  }

  // probe
  if (*context.probe_subcommand) {
    return run_probe_command(context.probe_opts);
  }

  // build
  if (*context.build_subcommand) {
    return run_build_command(context.build_opts, context.build_subcommand);
  }

  // diarize (diagnostic)
  if (*context.diarize_subcommand) {
    return run_diarize_command(context.diarize_opts);
  }

  // diarize-replay (diagnostic)
  if (*context.diarize_replay_subcommand) {
    return run_diarize_replay_command(context.diarize_replay_opts);
  }

  return 0;
}
