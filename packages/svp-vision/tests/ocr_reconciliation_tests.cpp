// Reconciling readings into observations: one observation per line on screen,
// named by its most-read text, however the recognizer clipped or misread it.

#include "../src/ocr_generation/ocr_generation_internal.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace internal = svp::vision::ocr_generation_internal;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

struct Box {
  int left, top, right, bottom;
};

constexpr Box kLine{100, 200, 900, 240};
constexpr Box kNextLine{100, 260, 900, 300};

internal::FrameDetection reading(int second, const std::string& text, Box box = kLine) {
  internal::FrameDetection detection;
  detection.frame_id = "frame_" + std::to_string(second);
  detection.timestamp_us = static_cast<std::int64_t>(second) * 1'000'000;
  detection.frame_index = static_cast<std::size_t>(second);
  detection.frame_width = 1920;
  detection.frame_height = 1080;
  detection.raw_text = text;
  detection.confidence = 0.9;
  detection.bbox_left = box.left;
  detection.bbox_top = box.top;
  detection.bbox_right = box.right;
  detection.bbox_bottom = box.bottom;
  return detection;
}

std::vector<std::string> texts(const std::vector<internal::ReconciledObservation>& observations) {
  std::vector<std::string> out;
  for (const auto& observation : observations) out.push_back(observation.raw_text);
  return out;
}

void test_clipped_and_misread_readings_of_a_line_are_one_observation() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "If someone were to buy this park today"),
       reading(1, "f someone were to buy this park today"),       // clipped first letter
       reading(2, "If someone were to buy this park todav"),      // misread letter
       reading(2, "ouy this park today", Box{400, 200, 900, 240}),  // clipped, edge misread
       reading(3, "If someone were to buy this park today"),
       reading(3, "Level 2 Investor", kNextLine)},
      4);
  require(texts(reconciled) ==
              std::vector<std::string>{"If someone were to buy this park today", "Level 2 Investor"},
          "one observation per line, named by its most-read text");
  require(reconciled[0].detection_count == 5 && reconciled[0].source_frame_ids.size() == 5,
          "every reading of the line counts as a sighting");
  require(reconciled[0].start_us == 0 && reconciled[0].end_us == 3'000'000,
          "the line spans all its readings");
}

void test_different_numbers_in_the_same_place_stay_apart() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "Total profit $30,000"), reading(1, "Total profit $80,000"),
       reading(2, "Total profit $30,000"), reading(3, "Total profit $80,000")},
      4);
  require(texts(reconciled).size() == 2, "a changed number is never merged away");
}

void test_new_text_in_the_same_place_stays_apart() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "Level 1: buys and operates as is"),
       reading(1, "Level 1: buys and operates as is"),
       reading(2, "Level 3: maximum reinvestment"),  // the next slide, same spot
       reading(3, "Level 3: maximum reinvestment")},
      4);
  require(texts(reconciled).size() == 2, "different text in the same place is its own line");
}

void test_near_identical_text_far_apart_in_time_is_not_merged() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "Buy this park today"), reading(1, "Buy this park today"),
       reading(30, "Buy this park todav"), reading(31, "Buy this park todav")},
      40);
  require(texts(reconciled).size() == 2,
          "near-identical readings join only within a few seconds of each other");
}

}  // namespace

int main() {
  test_clipped_and_misread_readings_of_a_line_are_one_observation();
  test_different_numbers_in_the_same_place_stay_apart();
  test_new_text_in_the_same_place_stays_apart();
  test_near_identical_text_far_apart_in_time_is_not_merged();
  std::cout << "ocr reconciliation tests passed\n";
  return 0;
}
