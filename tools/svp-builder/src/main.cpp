#include "svp/core/version.hpp"
#include "svp/media/media_ingest_plan.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

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

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"SVP builder"};
  app.set_version_flag("--version", svp::core::tool_version_label("svp-builder"));
  app.require_subcommand(0, 1);

  std::string probe_source_path;
  std::string probe_json_path;
  bool probe_json_output = false;

  auto* probe = app.add_subcommand(
      "probe", "Read deterministic media probe metadata and compute SVP timing data");
  probe->add_option("source", probe_source_path, "Source media path")->required();
  probe->add_option("--probe-json", probe_json_path,
                    "Precomputed media probe JSON; native FFmpeg probing is not linked yet")
      ->required();
  probe->add_flag("--json", probe_json_output, "Emit JSON");

  std::string build_source_path;
  std::string build_probe_json_path;
  std::string build_output_path;
  std::string stop_after = "media-ingest";

  auto* build = app.add_subcommand(
      "build", "Write an honest media-ingest foundation JSON artifact");
  build->add_option("source", build_source_path, "Source media path")->required();
  build->add_option("--probe-json", build_probe_json_path,
                    "Precomputed media probe JSON; native FFmpeg probing is not linked yet")
      ->required();
  build->add_option("--out", build_output_path,
                    "Output path for the media-ingest foundation JSON")
      ->required();
  build->add_option("--stop-after", stop_after,
                    "Only media-ingest is supported by this foundation command");

  CLI11_PARSE(app, argc, argv);

  try {
    if (*probe) {
      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(probe_source_path,
                                              svp::media::load_media_probe_json(
                                                  probe_json_path));
      if (probe_json_output) {
        std::cout << svp::media::media_ingest_plan_to_json(plan).dump(2) << "\n";
      } else {
        print_plan_summary(plan);
      }
      return 0;
    }

    if (*build) {
      if (stop_after != "media-ingest") {
        std::cerr << "svp-builder build currently supports only --stop-after media-ingest\n";
        return 2;
      }

      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(build_source_path,
                                              svp::media::load_media_probe_json(
                                                  build_probe_json_path));
      nlohmann::json output = svp::media::media_ingest_plan_to_json(plan);
      output["builder_command"] = {
          {"command", "build"},
          {"stop_after", "media-ingest"},
          {"valid_svp_package_written", false},
      };
      write_json_file(build_output_path, output);
      std::cout << "Wrote media-ingest foundation JSON: " << build_output_path << "\n";
      std::cout << "No .svp package was created by this foundation command.\n";
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
