#include "svp/builder/build_pipeline.hpp"

#include "default_staging.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace svp::builder {

std::vector<std::string_view> supported_build_stage_names() {
  return {"media-ingest", "audio", "vision-plan", "foundation-color",
          "foundation-ocr", "package"};
}

std::optional<BuildStage> parse_build_stage(std::string_view value) {
  if (value == "media-ingest") return BuildStage::media_ingest;
  if (value == "audio") return BuildStage::audio;
  if (value == "vision-plan") return BuildStage::vision_plan;
  if (value == "foundation-color") return BuildStage::foundation_color;
  if (value == "foundation-ocr") return BuildStage::foundation_ocr;
  if (value == "package") return BuildStage::package_skeleton;
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
      return "package";
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

BuilderConcurrencyPolicy builder_concurrency_policy(
    const svp::vision::InferencePerformanceOptions& performance,
    std::size_t requested_batch_jobs) {
  const std::size_t hardware =
      std::max<std::size_t>(1, std::thread::hardware_concurrency());

  // Single-video work overlaps the two independent heavy lanes: audio/ASR and
  // vision/OCR. Heavy stage internals keep their own existing worker policies.
  constexpr std::size_t kSingleVideoHeavyLaneCap = 2;

  // Batch caps are deliberately lower than raw hardware concurrency because a
  // single item can already run ASR beside OCR, and fast OCR widens recognition
  // workers through the existing OCR profile policy.
  constexpr std::size_t kSerialOcrBatchHardwareDivisor = 2;
  constexpr std::size_t kBackgroundOcrBatchHardwareDivisor = 3;
  constexpr std::size_t kFastOcrBatchHardwareDivisor = 4;

  std::size_t divisor = kBackgroundOcrBatchHardwareDivisor;
  if (performance.ocr_performance_profile == "serial") {
    divisor = kSerialOcrBatchHardwareDivisor;
  } else if (performance.ocr_performance_profile == "fast") {
    divisor = kFastOcrBatchHardwareDivisor;
  }

  const std::size_t cap = std::max<std::size_t>(1, hardware / divisor);
  const std::size_t requested = std::max<std::size_t>(1, requested_batch_jobs);
  return {
      .single_video_heavy_lanes = std::min(kSingleVideoHeavyLaneCap, hardware),
      .max_batch_jobs = std::max<std::size_t>(1, std::min(requested, cap)),
  };
}

std::filesystem::path default_staging_dir_for_output(
    const std::filesystem::path& /*output_path*/) {
  return make_default_staging_dir();
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
