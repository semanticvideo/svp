#include "svp/builder/build_pipeline.hpp"

#include <stdexcept>

namespace svp::builder {

std::vector<std::string_view> supported_build_stage_names() {
  return {"media-ingest", "audio", "vision-plan", "foundation-color",
          "foundation-ocr", "package-skeleton"};
}

std::optional<BuildStage> parse_build_stage(std::string_view value) {
  if (value == "media-ingest") return BuildStage::media_ingest;
  if (value == "audio") return BuildStage::audio;
  if (value == "vision-plan") return BuildStage::vision_plan;
  if (value == "foundation-color") return BuildStage::foundation_color;
  if (value == "foundation-ocr") return BuildStage::foundation_ocr;
  if (value == "package-skeleton") return BuildStage::package_skeleton;
  return std::nullopt;
}

std::string_view build_stage_name(BuildStage stage) {
  switch (stage) {
    case BuildStage::media_ingest:
      return "media-ingest";
    case BuildStage::audio:
      return "audio";
    case BuildStage::vision_plan:
      return "vision-plan";
    case BuildStage::foundation_color:
      return "foundation-color";
    case BuildStage::foundation_ocr:
      return "foundation-ocr";
    case BuildStage::package_skeleton:
      return "package-skeleton";
  }
  throw std::runtime_error("unknown build stage");
}

BuildStageExecutionPlan execution_plan_for_stage(BuildStage stage) {
  switch (stage) {
    case BuildStage::media_ingest:
      return {};
    case BuildStage::audio:
      return {.run_audio = true};
    case BuildStage::vision_plan:
      return {.run_vision_plan = true};
    case BuildStage::foundation_color:
      return {.run_foundation_color = true};
    case BuildStage::foundation_ocr:
      return {.run_foundation_ocr = true};
    case BuildStage::package_skeleton:
      return {.run_audio = true,
              .run_foundation_color = true,
              .run_package_skeleton = true};
  }
  throw std::runtime_error("unknown build stage");
}

std::filesystem::path default_staging_dir_for_output(
    const std::filesystem::path& output_path) {
  return std::filesystem::path(output_path.string() + ".staging");
}

BuildOutputPaths resolve_package_skeleton_output_paths(
    const std::filesystem::path& output_path) {
  if (output_path.extension() == ".svp") {
    return {.package_path = output_path,
            .json_output_path = std::filesystem::path(output_path.string() + ".json")};
  }
  std::filesystem::path package_path = output_path;
  package_path.replace_extension(".svp");
  return {.package_path = package_path, .json_output_path = output_path};
}


}  // namespace svp::builder
