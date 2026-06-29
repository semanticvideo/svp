#include "svp/builder/build_pipeline.hpp"
#include "svp/core/version.hpp"
#include "svp/media/media_ingest_plan.hpp"

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

  CLI11_PARSE(app, argc, argv);

  try {
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
