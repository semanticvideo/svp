#include "svp/builder/build_pipeline.hpp"

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

namespace {

void test_supported_stage_names_are_stable_and_ordered() {
  const std::vector<std::string_view> names =
      svp::builder::supported_build_stage_names();

  assert((names == std::vector<std::string_view>{
                       "media-ingest",
                       "audio",
                       "vision-plan",
                       "foundation-color",
                       "foundation-ocr",
                       "package-skeleton",
                   }));
}

void test_stage_name_round_trip() {
  for (std::string_view name : svp::builder::supported_build_stage_names()) {
    const std::optional<svp::builder::BuildStage> stage =
        svp::builder::parse_build_stage(name);
    assert(stage.has_value());
    assert(svp::builder::build_stage_name(*stage) == name);
  }

  assert(!svp::builder::parse_build_stage(""));
  assert(!svp::builder::parse_build_stage("package"));
  assert(!svp::builder::parse_build_stage("foundation-audio"));
}

void test_execution_plan_preserves_existing_stage_conditions() {
  using svp::builder::BuildStage;
  using svp::builder::execution_plan_for_stage;

  auto media_ingest = execution_plan_for_stage(BuildStage::media_ingest);
  assert(!media_ingest.run_audio);
  assert(!media_ingest.run_vision_plan);
  assert(!media_ingest.run_foundation_color);
  assert(!media_ingest.run_foundation_ocr);
  assert(!media_ingest.run_package_skeleton);

  auto audio = execution_plan_for_stage(BuildStage::audio);
  assert(audio.run_audio);
  assert(!audio.run_vision_plan);
  assert(!audio.run_foundation_color);
  assert(!audio.run_foundation_ocr);
  assert(!audio.run_package_skeleton);

  auto vision_plan = execution_plan_for_stage(BuildStage::vision_plan);
  assert(!vision_plan.run_audio);
  assert(vision_plan.run_vision_plan);
  assert(!vision_plan.run_foundation_color);
  assert(!vision_plan.run_foundation_ocr);
  assert(!vision_plan.run_package_skeleton);

  auto foundation_color = execution_plan_for_stage(BuildStage::foundation_color);
  assert(!foundation_color.run_audio);
  assert(!foundation_color.run_vision_plan);
  assert(foundation_color.run_foundation_color);
  assert(!foundation_color.run_foundation_ocr);
  assert(!foundation_color.run_package_skeleton);

  auto foundation_ocr = execution_plan_for_stage(BuildStage::foundation_ocr);
  assert(!foundation_ocr.run_audio);
  assert(!foundation_ocr.run_vision_plan);
  assert(!foundation_ocr.run_foundation_color);
  assert(foundation_ocr.run_foundation_ocr);
  assert(!foundation_ocr.run_package_skeleton);

  auto package_skeleton = execution_plan_for_stage(BuildStage::package_skeleton);
  assert(package_skeleton.run_audio);
  assert(!package_skeleton.run_vision_plan);
  assert(package_skeleton.run_foundation_color);
  assert(!package_skeleton.run_foundation_ocr);
  assert(package_skeleton.run_package_skeleton);
}

void test_default_staging_dir_matches_existing_cli_contract() {
  const std::filesystem::path output_path = "out/example.foundation.json";
  assert(svp::builder::default_staging_dir_for_output(output_path) ==
         std::filesystem::path("out/example.foundation.json.staging"));
}

void test_package_skeleton_output_path_resolution() {
  auto svp_output =
      svp::builder::resolve_package_skeleton_output_paths("out/video.svp");
  assert(svp_output.package_path == std::filesystem::path("out/video.svp"));
  assert(svp_output.json_output_path == std::filesystem::path("out/video.svp.json"));

  auto json_output =
      svp::builder::resolve_package_skeleton_output_paths("out/video.json");
  assert(json_output.package_path == std::filesystem::path("out/video.svp"));
  assert(json_output.json_output_path == std::filesystem::path("out/video.json"));

  auto extensionless_output =
      svp::builder::resolve_package_skeleton_output_paths("out/video");
  assert(extensionless_output.package_path == std::filesystem::path("out/video.svp"));
  assert(extensionless_output.json_output_path == std::filesystem::path("out/video"));
}

}  // namespace

int main() {
  test_supported_stage_names_are_stable_and_ordered();
  test_stage_name_round_trip();
  test_execution_plan_preserves_existing_stage_conditions();
  test_default_staging_dir_matches_existing_cli_contract();
  test_package_skeleton_output_path_resolution();
  return 0;
}
