#include "build_selected_output.hpp"

#include "cli_context.hpp"

#include "svp/builder/embedded_svpi_transport.hpp"
#include "svp/builder/interlace.hpp"

#include <filesystem>
#include <iostream>

namespace {

svp::builder::InterlaceCreateOptions make_create_options(
    const BuildCliOptions& options,
    const std::filesystem::path& output_path,
    const std::shared_ptr<svp::builder::BuildProgressSink>& progress_sink) {
  svp::builder::InterlaceCreateOptions create;
  create.source_path = options.source_path;
  create.output_path = output_path.string();
  create.staging_dir = options.staging_dir;
  create.model_cache_dir = options.model_cache_dir;
  create.ffprobe_path = options.ffprobe_path;
  create.ffmpeg_path = options.ffmpeg_path;
  create.probe_json_path = options.probe_json_path;
  create.sherpa_lib_path = options.sherpa_lib_path;
  create.performance = options.performance;
  create.allow_fallback_diarization = options.allow_fallback_diarization;
  create.force_single_speaker = options.force_single_speaker;
  create.serial_pipeline = options.serial_pipeline;
  create.progress_sink = progress_sink;
  return create;
}

}  // namespace

int run_selected_output_build(
    const BuildCliOptions& options,
    const std::shared_ptr<svp::builder::BuildProgressSink>& progress_sink) {
  if (options.stop_after != "package") {
    std::cerr << "--output-format " << options.output_format
              << " requires the complete package stage\n";
    return 2;
  }
  if (std::filesystem::exists(options.output_path) && !options.overwrite) {
    std::cerr << "output already exists; pass --overwrite to replace it\n";
    return 1;
  }

  if (options.output_format == "svpi") {
    const auto result = svp::builder::interlace_create(
        make_create_options(options, options.output_path, progress_sink));
    if (!result.success) {
      std::cerr << "SVPI build failed: " << result.error_message << "\n";
      return 1;
    }
    std::cout << "SVPI created: " << result.svpi_path.string() << "\n";
    return 0;
  }

  svp::builder::EmbeddedTransportBuildOptions build;
  build.svpi_options = make_create_options(
      options, std::filesystem::path{}, progress_sink);
  build.output_path = options.output_path;
  build.overwrite_output = options.overwrite;
  const auto result = svp::builder::build_embedded_svpi_transport(build);
  if (!result.success) {
    std::cerr << "embedded SVPI transport build failed: "
              << result.error_message << "\n";
    return 1;
  }
  std::cout << "Embedded SVPI transport created: "
            << result.output_path.string() << "\n";
  return 0;
}
