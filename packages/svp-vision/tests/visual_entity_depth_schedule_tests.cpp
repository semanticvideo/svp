#include "svp/vision/visual_entity_depth_schedule.hpp"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

svp::vision::ColorRasterFrame frame(std::int64_t pts_us, std::uint8_t value) {
  svp::vision::ColorRasterFrame result;
  result.timestamp_us = pts_us;
  result.width = 4;
  result.height = 4;
  result.pixels.assign(16, {value, value, value});
  return result;
}

void test_periodic_and_change_burst_schedule() {
  std::vector<svp::vision::ColorRasterFrame> frames;
  for (int index = 0; index < 10; ++index) {
    frames.push_back(frame(index * 200000, index < 4 ? 20 : 220));
  }
  const auto selected =
      svp::vision::select_visual_entity_depth_frames(frames);
  const std::vector<std::size_t> expected = {0, 4, 5, 6};
  check(selected == expected,
        "periodic sampling and three-frame edit burst are combined");
}

void test_ordinary_motion_does_not_trigger_burst() {
  const std::vector<svp::vision::ColorRasterFrame> frames = {
      frame(0, 100), frame(200000, 105), frame(400000, 110)};
  const auto selected =
      svp::vision::select_visual_entity_depth_frames(frames);
  check(selected == std::vector<std::size_t>{0},
        "small frame changes do not trigger dense depth inference");
}

}  // namespace

int main() {
  test_periodic_and_change_burst_schedule();
  test_ordinary_motion_does_not_trigger_burst();
  return failures == 0 ? 0 : 1;
}
