// The builder maps --stop-after, the tracking quality, and model runtime
// availability onto the frame plan exactly as the stages decide what to decode.

#include "build_frame_plan.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/frame_plan.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

svp::media::MediaIngestPlan media_plan(std::int64_t duration_us) {
  svp::media::MediaIngestPlan plan;
  plan.primary_video_stream.width = 1920;
  plan.primary_video_stream.height = 1080;
  plan.primary_video_stream.timing.timebase = {1, 1'000'000};
  plan.primary_video_stream.timing.duration_pts = duration_us;
  plan.canonical_raster =
      svp::media::compute_canonical_analysis_raster({1920, 1080, 0, {1, 1}});
  return plan;
}

std::set<std::string> purposes_of(const svp::vision::FrameCatalog& catalog) {
  std::set<std::string> purposes;
  for (const auto& entry : catalog.planned_entries())
    purposes.insert(entry.purposes.begin(), entry.purposes.end());
  return purposes;
}

void test_stop_after_stage_selection() {
  using svp::builder::BuildStage;
  struct Case {
    BuildStage stage;
    std::set<std::string> purposes;
  };
  const std::vector<Case> cases = {
      {BuildStage::media_ingest, {}},
      {BuildStage::audio, {}},
      {BuildStage::vision_plan, {}},
      {BuildStage::foundation_color, {"color"}},
      {BuildStage::foundation_ocr, {"canonical", "ocr"}},
      {BuildStage::package_skeleton,
       {"color", "canonical", "ocr", "visual_entity_tracking"}},
  };
  const auto media = media_plan(30'150'000);
  for (const auto& c : cases) {
    const auto stage_plan = svp::builder::execution_plan_for_stage(c.stage);
    svp::vision::FrameCatalog catalog;
    svp::builder::plan_build_frames(catalog, stage_plan, media, "medium",
                                    /*model_runtime_available=*/true);
    const std::string name(svp::builder::build_stage_name(c.stage));
    require(catalog.locked_to_plan(), "catalog locked for " + name);
    require(purposes_of(catalog) == c.purposes,
            "planned stage purposes for --stop-after " + name);
  }
}

void test_visual_tracking_gates() {
  const auto media = media_plan(30'150'000);
  const auto package = svp::builder::execution_plan_for_stage(
      svp::builder::BuildStage::package_skeleton);

  const auto off = svp::builder::build_frame_plan_inputs(
      package, media, "off", true, std::nullopt);
  require(off.visual_tracking == svp::vision::VisualTrackingQuality::off,
          "quality off plans no tracking");

  const auto no_runtime = svp::builder::build_frame_plan_inputs(
      package, media, "high", false, std::nullopt);
  require(no_runtime.visual_tracking ==
              svp::vision::VisualTrackingQuality::off,
          "tracking is skipped without the model runtime");
  require(no_runtime.color && no_runtime.canonical && no_runtime.ocr,
          "color, canonical, and OCR still run without the model runtime");

  for (const auto quality : {"low", "medium", "high"}) {
    const auto inputs = svp::builder::build_frame_plan_inputs(
        package, media, quality, true, std::nullopt);
    require(inputs.visual_tracking ==
                svp::vision::parse_visual_tracking_quality(quality),
            std::string("tracking quality carried through: ") + quality);
  }
}

void test_ocr_override_reaches_plan() {
  const auto media = media_plan(30'150'000);
  const auto ocr_stage = svp::builder::execution_plan_for_stage(
      svp::builder::BuildStage::foundation_ocr);
  const std::vector<std::int64_t> override_us = {0, 12'345'678};
  const auto inputs = svp::builder::build_frame_plan_inputs(
      ocr_stage, media, "medium", true, override_us);
  svp::vision::FrameCatalog catalog;
  svp::vision::load_frame_plan(
      catalog, svp::vision::plan_frame_registrations(inputs));
  std::vector<std::int64_t> ocr_timestamps;
  for (const auto& entry : catalog.planned_entries()) {
    if (entry.purposes.count("ocr")) ocr_timestamps.push_back(entry.timestamp_us);
  }
  require(ocr_timestamps == override_us,
          "the OCR diagnostic override replaces the OCR sample plan");
}

}  // namespace

int main() {
  test_stop_after_stage_selection();
  test_visual_tracking_gates();
  test_ocr_override_reaches_plan();
  if (g_failures != 0) {
    std::cerr << g_failures << " builder frame plan check(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "builder frame plan tests passed\n";
  return EXIT_SUCCESS;
}
