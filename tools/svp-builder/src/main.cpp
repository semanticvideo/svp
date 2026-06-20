#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/vad_execution_boundary.hpp"
#include "svp/core/version.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/observation_pipeline_plan.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
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

svp::media::MediaProbe load_or_run_probe(const std::string& source_path,
                                         const std::string& probe_json_path,
                                         const std::string& ffprobe_path) {
  if (!probe_json_path.empty()) {
    return svp::media::load_media_probe_json(probe_json_path);
  }
  return svp::media::probe_media_with_ffprobe(source_path, ffprobe_path);
}

bool executable_exists(const std::filesystem::path& executable_path) {
  if (executable_path.empty()) {
    return false;
  }
  if (executable_path.has_parent_path()) {
    return std::filesystem::exists(executable_path);
  }

  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return false;
  }

  std::string paths(path_env);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = paths.find(':', start);
    const std::string entry =
        paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty() && std::filesystem::exists(std::filesystem::path(entry) / executable_path)) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return false;
}

std::filesystem::path default_staging_dir_for_output(
    const std::filesystem::path& output_path) {
  return std::filesystem::path(output_path.string() + ".staging");
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
  std::string stop_after = "media-ingest";

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
  build->add_option("--stop-after", stop_after,
                    "Supported foundation stages: media-ingest, audio, vision-plan");

  CLI11_PARSE(app, argc, argv);

  try {
    if (*probe) {
      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(probe_source_path,
                                              load_or_run_probe(probe_source_path,
                                                                probe_json_path,
                                                                probe_ffprobe_path));
      if (probe_json_output) {
        std::cout << svp::media::media_ingest_plan_to_json(plan).dump(2) << "\n";
      } else {
        print_plan_summary(plan);
      }
      return 0;
    }

    if (*build) {
      if (stop_after != "media-ingest" && stop_after != "audio" &&
          stop_after != "vision-plan") {
        std::cerr << "svp-builder build currently supports --stop-after media-ingest, audio, "
                     "or vision-plan\n";
        return 2;
      }

      const svp::media::MediaIngestPlan plan =
          svp::media::build_media_ingest_plan(build_source_path,
                                              load_or_run_probe(build_source_path,
                                                                build_probe_json_path,
                                                                build_ffprobe_path));
      nlohmann::json output = svp::media::media_ingest_plan_to_json(plan);
      if (stop_after == "audio") {
        const svp::audio::AudioStagePlan audio_plan =
            svp::audio::build_audio_stage_plan(build_source_path,
                                               plan.probe,
                                               executable_exists(build_ffmpeg_path),
                                               build_ffmpeg_path);
        nlohmann::json audio_json = svp::audio::audio_stage_plan_to_json(audio_plan);
        const std::filesystem::path staging_dir =
            build_staging_dir.empty()
                ? default_staging_dir_for_output(build_output_path)
                : std::filesystem::path(build_staging_dir);
        const svp::audio::AudioExtractionRun extraction_run =
            svp::audio::execute_audio_extraction_plan(audio_plan.extraction_plan,
                                                      staging_dir);
        nlohmann::json extraction_run_json =
            svp::audio::audio_extraction_run_to_json(extraction_run);
        const svp::audio::VadExecutionBoundary vad_boundary =
            svp::audio::build_vad_execution_boundary(audio_plan.vad_task_plan,
                                                     extraction_run.analysis_audio_written,
                                                     extraction_run.waveform_written,
                                                     false);
        audio_json["audio_extraction"]["execution"] = extraction_run_json;
        audio_json["audio_extraction"]["extraction_run"] =
            extraction_run.extraction_run;
        audio_json["audio_extraction"]["original_streams_written"] =
            extraction_run.original_streams_written;
        audio_json["audio_extraction"]["analysis_audio_written"] =
            extraction_run.analysis_audio_written;
        audio_json["audio_extraction"]["audio_absence_written"] =
            extraction_run.audio_absence_written;
        audio_json["audio_extraction"]["waveform_written"] =
            extraction_run.waveform_written;
        audio_json["audio_extraction"]["processor_provenance_written"] =
            extraction_run.processor_provenance_written;
        for (const std::string& blocker : extraction_run.blockers) {
          audio_json["blockers"].push_back(blocker);
        }
        audio_json["vad_execution_boundary"] =
            svp::audio::vad_execution_boundary_to_json(vad_boundary);
        output["audio_foundation"] = audio_json;
      } else if (stop_after == "vision-plan") {
        const svp::vision::VisionObservationPipelinePlan vision_plan =
            svp::vision::build_vision_observation_pipeline_plan(plan);
        output["vision_observation_pipeline"] =
            svp::vision::vision_observation_pipeline_plan_to_json(vision_plan);
      }
      output["builder_command"] = {
          {"command", "build"},
          {"stop_after", stop_after},
          {"valid_svp_package_written", false},
      };
      write_json_file(build_output_path, output);
      std::cout << "Wrote builder foundation JSON: " << build_output_path << "\n";
      if (stop_after == "audio") {
        std::cout << "Audio task plan only; no transcription or diarization was generated.\n";
      }
      if (stop_after == "vision-plan") {
        std::cout << "Vision/OCR/color task plan only; no observations were generated.\n";
      }
      std::cout << "No .svp package was created by this foundation command.\n";
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
