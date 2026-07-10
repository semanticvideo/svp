#include "build_selected_output.hpp"

#include "cli_context.hpp"

#include "svp/builder/embedded_svpi_transport.hpp"
#include "svp/builder/interlace.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#include <unistd.h>

namespace {

class TemporaryBuildDirectory {
 public:
  TemporaryBuildDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "svp-embedded-build-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (mkdtemp(writable.data()) != nullptr) {
      path_ = writable.data();
    }
  }

  ~TemporaryBuildDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

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

  TemporaryBuildDirectory temporary;
  if (temporary.path().empty()) {
    std::cerr << "unable to create managed temporary directory for canonical SVPI\n";
    return 1;
  }
  const auto temporary_svpi = temporary.path() / "semantic-package.svpi";
  const auto svpi = svp::builder::interlace_create(
      make_create_options(options, temporary_svpi, progress_sink));
  if (!svpi.success) {
    std::cerr << "canonical SVPI build failed: " << svpi.error_message << "\n";
    return 1;
  }

  svp::builder::EmbeddedTransportEmbedOptions embed;
  embed.container_path = options.source_path;
  embed.svpi_path = temporary_svpi;
  embed.output_path = options.output_path;
  embed.ffprobe_path = options.ffprobe_path;
  embed.overwrite_output = options.overwrite;
  embed.progress_sink = progress_sink;
  const auto result = svp::builder::embed_svpi_transport(embed);
  if (!result.success) {
    std::cerr << "embedded SVPI transport build failed: "
              << result.error_message << "\n";
    return 1;
  }
  std::cout << "Embedded SVPI transport created: "
            << result.output_path.string() << "\n";
  return 0;
}
