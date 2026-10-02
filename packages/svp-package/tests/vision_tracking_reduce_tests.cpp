// The tracking stage's reduce step folds window outcomes one at a time: it
// asks for each window once, in window order, and only after the window
// before it has been folded, so the coordinator holds one decoded window at a
// time whatever the video's length (as the stage itself does).

#include "svp/media/media_ingest_plan.hpp"
#include "svp/package/vision_lane_stages.hpp"
#include "svp/vision/visual_entity_sampling.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void test_windows_are_folded_one_at_a_time() {
  const std::filesystem::path staging =
      std::filesystem::temp_directory_path() / "svp-tracking-reduce-test";
  std::filesystem::remove_all(staging);
  std::filesystem::create_directories(staging);
  const svp::media::MediaIngestPlan media;
  svp::package::VisionLaneSettings settings;
  settings.staging_dir = staging;
  settings.model_runtime_available = true;
  settings.media_plan = &media;
  settings.ffmpeg_path = "ffmpeg";
  settings.visual_tracking_quality = "medium";

  for (const std::int64_t duration_us : {30'000'000LL, 600'000'000LL}) {
    const auto policy =
        svp::vision::visual_tracking_quality_policy(svp::vision::VisualTrackingQuality::medium);
    svp::vision::VisualEntityPipelinePlan plan;
    plan.sampling = svp::vision::visual_entity_sampling_options(policy);
    plan.depth_schedule.periodic_interval_us = policy.depth_interval_us;
    plan.duration_us = duration_us;
    plan.windows = svp::vision::make_visual_entity_sampling_plan(duration_us, plan.sampling);

    std::size_t folded = 0;
    std::vector<std::size_t> asked;
    bool asked_early = false;
    const svp::package::SpatialProgressCallback progress =
        [&](const char* stage, std::size_t current, std::size_t, const char*) {
          if (std::string(stage) == "visual_tracking") folded = current;
        };
    const svp::package::VisualEntityWindowSource source = [&](std::size_t index) {
      asked.push_back(index);
      asked_early = asked_early || folded != index;
      svp::vision::VisualEntityWindowOutcome outcome;
      outcome.failures = {{"frame_decode", "no frames"}};
      return outcome;
    };
    (void)svp::package::run_vision_tracking_reduce_stage(settings, plan, source, {}, nullptr,
                                                         progress);
    std::vector<std::size_t> expected(plan.windows.size());
    for (std::size_t index = 0; index < expected.size(); ++index) expected[index] = index;
    check(asked == expected, "each window is asked for once, in window order");
    check(!asked_early, "a window is asked for only after the one before it was folded");
    check(folded == plan.windows.size(), "every window was folded");
  }
  std::filesystem::remove_all(staging);
}

}  // namespace

int main() {
  test_windows_are_folded_one_at_a_time();
  if (failures != 0) return 1;
  std::cout << "All tracking reduce tests passed.\n";
  return 0;
}
