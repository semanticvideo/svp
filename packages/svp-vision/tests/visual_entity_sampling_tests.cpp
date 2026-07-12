#include "svp/vision/visual_entity_sampling.hpp"

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

void test_short_media_has_complete_coverage() {
  const auto windows =
      svp::vision::make_visual_entity_sampling_plan(1750000);
  check(windows.size() == 1, "short media uses one window");
  check(windows.front().start_us == 0, "coverage begins at zero");
  check(windows.front().end_us == 1750000, "coverage reaches duration");
  check(windows.front().timestamps_us.front() == 0,
        "first timestamp is zero");
  check(windows.front().timestamps_us.back() == 1750000,
        "last timestamp is duration");
  check_maximum_gap(windows.front(), 200000);
}

void test_long_media_is_bounded_and_overlapping() {
  const auto windows =
      svp::vision::make_visual_entity_sampling_plan(17660000);
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
  svp::vision::VisualEntitySamplingOptions options;
  options.window_overlap_us = 0;
  bool rejected = false;
  try {
    (void)svp::vision::make_visual_entity_sampling_plan(1000000, options);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "overlap without handoff evidence is rejected");
}

}  // namespace

int main() {
  test_short_media_has_complete_coverage();
  test_long_media_is_bounded_and_overlapping();
  test_invalid_policy_is_rejected();
  return failures == 0 ? 0 : 1;
}
