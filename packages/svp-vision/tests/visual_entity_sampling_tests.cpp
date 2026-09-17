#include "svp/vision/visual_entity_sampling.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void check_maximum_gap(
    const svp::vision::VisualEntitySamplingWindow& window,
    std::int64_t maximum_gap_us) {
  for (std::size_t index = 1; index < window.timestamps_us.size(); ++index) {
    check(window.timestamps_us[index] - window.timestamps_us[index - 1] <=
              maximum_gap_us,
          "sampling gap exceeds policy");
  }
}

void test_short_media_stays_on_the_requested_cadence() {
  const svp::vision::VisualEntitySamplingOptions options{
      200000, 5000000, 1000000};
  const auto windows =
      svp::vision::make_visual_entity_sampling_plan(1750000, options);
  check(windows.size() == 1, "short media uses one window");
  check(windows.front().start_us == 0, "coverage begins at zero");
  check(windows.front().end_us == 1750000, "coverage reaches duration");
  check(windows.front().timestamps_us.front() == 0,
        "first timestamp is zero");
  check(windows.front().timestamps_us.back() == 1600000,
        "off-grid duration does not create a mislabeled sample");
  for (const auto timestamp_us : windows.front().timestamps_us) {
    check(timestamp_us % 200000 == 0,
          "every requested timestamp stays on the configured cadence");
  }
  check_maximum_gap(windows.front(), 200000);
}

void test_long_media_is_bounded_and_overlapping() {
  const svp::vision::VisualEntitySamplingOptions options{
      200000, 5000000, 1000000};
  const auto windows =
      svp::vision::make_visual_entity_sampling_plan(17660000, options);
  check(windows.size() == 5, "long media is split into bounded windows");
  check(windows.front().start_us == 0, "first window begins at zero");
  check(windows.back().end_us == 17660000,
        "last window reaches media duration");

  for (std::size_t index = 0; index < windows.size(); ++index) {
    const auto& window = windows[index];
    check(window.end_us - window.start_us <= 5000000,
          "window duration remains bounded");
    check(window.timestamps_us.size() <= 26,
          "window sample count remains bounded");
    check_maximum_gap(window, 200000);
    if (index > 0) {
      check(windows[index - 1].end_us - window.start_us == 1000000,
            "adjacent windows retain the configured overlap");
    }
  }
}

void test_invalid_policy_is_rejected() {
  const svp::vision::VisualEntitySamplingOptions options{
      200000, 5000000, 0};
  bool rejected = false;
  try {
    (void)svp::vision::make_visual_entity_sampling_plan(1000000, options);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "overlap without handoff evidence is rejected");
}

void test_visual_tracking_quality_owns_sampling_coverage() {
  using svp::vision::VisualTrackingQuality;
  using svp::vision::visual_tracking_quality_policy;

  const auto low = visual_tracking_quality_policy(VisualTrackingQuality::low);
  const auto medium =
      visual_tracking_quality_policy(VisualTrackingQuality::medium);
  const auto high = visual_tracking_quality_policy(VisualTrackingQuality::high);

  check(!svp::vision::visual_tracking_enabled(VisualTrackingQuality::off),
        "off disables visual tracking");
  check(svp::vision::visual_tracking_enabled(VisualTrackingQuality::low),
        "low enables visual tracking");
  check(low.sample_interval_us == 500000, "low quality uses two Hz");
  check(medium.sample_interval_us == 333333,
        "medium quality uses three Hz");
  check(high.sample_interval_us == 200000, "high quality uses five Hz");
  for (const auto policy : {low, medium, high}) {
    check(policy.window_duration_us == 20000000,
          "quality profiles use twenty-second windows");
    check(policy.window_overlap_us == 1000000,
          "quality profiles preserve identity handoff overlap");
    check(policy.depth_interval_us % policy.sample_interval_us == 0,
          "depth cadence remains aligned to RGB cadence");
  }

  check(svp::vision::parse_visual_tracking_quality("low") ==
            VisualTrackingQuality::low,
        "low quality parses");
  check(svp::vision::parse_visual_tracking_quality("off") ==
            VisualTrackingQuality::off,
        "off quality parses");
  check(svp::vision::parse_visual_tracking_quality("medium") ==
            VisualTrackingQuality::medium,
        "medium quality parses");
  check(svp::vision::parse_visual_tracking_quality("high") ==
            VisualTrackingQuality::high,
        "high quality parses");
  check(!svp::vision::parse_visual_tracking_quality("fast"),
        "performance profile names are not quality levels");
}

void test_off_returns_without_visual_tracking_work() {
  svp::media::MediaIngestPlan media_plan;
  svp::vision::VisualEntityPipelineOptions options;
  options.quality = svp::vision::VisualTrackingQuality::off;
  bool reported_progress = false;
  options.on_progress = [&](std::size_t, std::size_t) {
    reported_progress = true;
  };

  const auto result = svp::vision::run_visual_entity_pipeline(
      media_plan, "missing-ffmpeg", "missing-model-cache", {}, nullptr, options);

  check(result.windows_planned == 0, "off plans no tracking windows");
  check(result.windows_processed == 0, "off processes no tracking windows");
  check(result.frames_attempted == 0, "off attempts no tracking frames");
  check(!reported_progress, "off reports no tracking progress");
}

}  // namespace

int main() {
  test_short_media_stays_on_the_requested_cadence();
  test_long_media_is_bounded_and_overlapping();
  test_invalid_policy_is_rejected();
  test_visual_tracking_quality_owns_sampling_coverage();
  test_off_returns_without_visual_tracking_work();
  return failures == 0 ? 0 : 1;
}
