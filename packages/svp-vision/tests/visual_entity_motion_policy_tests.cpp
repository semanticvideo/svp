#include "../src/visual_entity_motion_policy.hpp"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void test_screen_spanning_fragments_are_a_transition() {
  const std::vector<cv::Rect> fragments = {
      {0, 0, 60, 40}, {40, 0, 60, 50}, {0, 40, 100, 60}};
  check(svp::vision::visual_entity_internal::has_global_motion_shape(
            fragments, 100, 100),
        "screen-spanning fragmented motion is a transition");
}

void test_localized_subject_is_not_a_transition() {
  const std::vector<cv::Rect> subject_parts = {
      {30, 20, 20, 20}, {45, 30, 20, 30}, {35, 55, 25, 20}};
  check(!svp::vision::visual_entity_internal::has_global_motion_shape(
             subject_parts, 100, 100),
        "localized subject parts remain entity evidence");
}

void test_one_large_subject_is_not_a_transition() {
  check(!svp::vision::visual_entity_internal::has_global_motion_shape(
             {{5, 5, 90, 90}}, 100, 100),
        "one large subject is not classified as a global edit");
}

}  // namespace

int main() {
  test_screen_spanning_fragments_are_a_transition();
  test_localized_subject_is_not_a_transition();
  test_one_large_subject_is_not_a_transition();
  return failures == 0 ? 0 : 1;
}
