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

constexpr Box kThirdLine{100, 320, 900, 360};
constexpr Box kFourthLine{100, 380, 900, 420};

// Lines start a second apart, so observations come out in line order.
void test_digits_read_as_lookalike_letters_join_their_line() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "Avg. Purchase Price: $2,065,512"),
       reading(1, "Avg. Purchase Price: $2.O65.512"),  // O for 0, . for ,
       reading(2, "Avg. Purchase Price: $2,065,512"),
       reading(1, "Flips: 1043", kNextLine),
       reading(2, "Flips: 1O43", kNextLine),
       reading(3, "Flips: 1043", kNextLine),
       reading(2, "Total Profit: -$58,412,107", kThirdLine),
       reading(3, "Total Profit: -$S8,4I2,l07", kThirdLine),  // S for 5, I and l for 1
       reading(4, "Total Profit: -$58,412,107", kThirdLine),
       reading(3, "Avg. Gross Profit: $1O0,8B0", kFourthLine),  // O for 0, B for 8
       reading(4, "Avg. Gross Profit: $100,880", kFourthLine),
       reading(5, "Avg. Gross Profit: $100,880", kFourthLine)},
      6);
  require(texts(reconciled) == std::vector<std::string>{"Avg. Purchase Price: $2,065,512",
                                                        "Flips: 1043",
                                                        "Total Profit: -$58,412,107",
                                                        "Avg. Gross Profit: $100,880"},
          "a digit misread as a look-alike letter is still the same line, named by its "
          "most-read text");
  for (const auto& observation : reconciled) {
    require(observation.detection_count == 3, "every reading of the line joins it");
  }
}

void test_letters_inside_words_stay_letters() {
  const auto reconciled = internal::reconcile_detections(
      {// "4.Benjamin": the B touches a number mark but is a word's letter, not an 8.
       reading(0, "4. Benjamin Nordstrom 12 flips"),
       reading(1, "4.Benjamin Nordstrorn 12 flips"),
       reading(2, "4. Benjamin Nordstrom 12 flips"),
       // "BOSS." has no digit: its S stays an S however close the period is.
       reading(1, "Top flipper: BOSS 12 flips", kNextLine),
       reading(2, "Top flipper: BOSS. 12 flipz", kNextLine),
       reading(3, "Top flipper: BOSS 12 flips", kNextLine),
       // "3.Ollie": the O touches a number mark but starts a word.
       reading(2, "3. Ollie Smith 7 flips", kThirdLine),
       reading(3, "3.Ollie Smitn 7 flips", kThirdLine),
       reading(4, "3. Ollie Smith 7 flips", kThirdLine),
       // SOLD is never 5010, so it never joins a reading of that number.
       reading(3, "Status: SOLD $250,000", kFourthLine),
       reading(4, "Status: 5010 $250,000", kFourthLine)},
      5);
  require(texts(reconciled) == std::vector<std::string>{"4. Benjamin Nordstrom 12 flips",
                                                        "Top flipper: BOSS 12 flips",
                                                        "3. Ollie Smith 7 flips",
                                                        "Status: SOLD $250,000",
                                                        "Status: 5010 $250,000"},
          "letters inside words stay letters: their readings still join, and a word never "
          "joins a number");
}

void test_a_leading_S_may_be_a_misread_dollar_sign() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "Total Profit: $774,120"), reading(1, "Total Profit: S774,120"),
       reading(2, "Total Profit: $774,120")},
      3);
  require(texts(reconciled) == std::vector<std::string>{"Total Profit: $774,120"},
          "an S that starts a number is not read as a 5, so a misread $ still joins its line");
}

void test_numbers_that_differ_after_reading_lookalikes_stay_apart() {
  const auto reconciled = internal::reconcile_detections(
      {reading(0, "Flips: 1O43"), reading(1, "Flips: 1048"), reading(2, "Flips: 1O43"),
       reading(3, "Flips: 1048"),
       reading(4, "Avg. Purchase Price: $2.O65.512", kNextLine),
       reading(5, "Avg. Purchase Price: $2,065,612", kNextLine)},
      6);
  require(texts(reconciled) == std::vector<std::string>{"Flips: 1O43", "Flips: 1048",
                                                        "Avg. Purchase Price: $2.O65.512",
                                                        "Avg. Purchase Price: $2,065,612"},
          "readings whose digits differ, look-alikes read as digits, never join");
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
  test_digits_read_as_lookalike_letters_join_their_line();
  test_letters_inside_words_stay_letters();
  test_a_leading_S_may_be_a_misread_dollar_sign();
  test_numbers_that_differ_after_reading_lookalikes_stay_apart();
  test_new_text_in_the_same_place_stays_apart();
  test_near_identical_text_far_apart_in_time_is_not_merged();
  std::cout << "ocr reconciliation tests passed\n";
  return 0;
}
