#include "svp/vision/visual_entity_cut_detection.hpp"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

svp::vision::ColorRasterFrame frame(
    std::int64_t timestamp_us,
    std::uint8_t value) {
  svp::vision::ColorRasterFrame result;
  result.timestamp_us = timestamp_us;
  result.width = 2;
  result.height = 2;
  result.pixels.assign(4, {value, value, value});
  return result;
}

void test_isolated_replacement_is_a_cut() {
  const auto evidence = svp::vision::detect_visual_entity_cuts({
      frame(0, 10), frame(200000, 100), frame(400000, 100)});
  check(evidence.size() == 2 && evidence[0].is_cut,
        "large replacement followed by stability is a cut");
}

void test_gradual_entrance_is_not_a_cut() {
  const auto evidence = svp::vision::detect_visual_entity_cuts({
      frame(0, 10), frame(200000, 60), frame(400000, 90),
      frame(600000, 100)});
  check(!evidence[0].is_cut,
        "multi-frame foreground entrance is not an isolated cut");
}

void test_replacement_can_settle_into_a_moving_scene() {
  const auto evidence = svp::vision::detect_visual_entity_cuts({
      frame(0, 10), frame(200000, 60), frame(400000, 68)});
  check(evidence.size() == 2 && evidence[0].is_cut,
        "a replacement followed by bounded new-shot motion is a cut");
}

void test_sustained_full_frame_transition_is_a_cut() {
  const auto evidence = svp::vision::detect_visual_entity_cuts({
      frame(0, 10), frame(200000, 50), frame(400000, 90),
      frame(600000, 130), frame(800000, 130)});
  check(evidence[0].is_cut && evidence[0].is_sustained_transition,
        "three sustained full-frame changes identify a transition boundary");
  check(!evidence[1].is_sustained_transition,
        "only the start of a sustained transition is marked");
}

}  // namespace

int main() {
  test_isolated_replacement_is_a_cut();
  test_gradual_entrance_is_not_a_cut();
  test_replacement_can_settle_into_a_moving_scene();
  test_sustained_full_frame_transition_is_a_cut();
  return failures == 0 ? 0 : 1;
}
