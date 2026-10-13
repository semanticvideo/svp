// Per-box recognition reuse in OCR frame batches: an unchanged box reuses its
// earlier reading, a changed or moved box is recognized again, the re-read
// cap holds, readings do not depend on how the plan is batched, and
// reconciliation counts reused sightings without letting them raise
// confidence.

#include "../src/ocr_generation/ocr_generation_internal.hpp"

#include "svp/vision/ocr_batch_policy.hpp"
#include "svp/vision/ocr_frame_batch.hpp"
#include "svp/vision/ocr_recognition_reuse.hpp"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace vision = svp::vision;
namespace internal = svp::vision::ocr_generation_internal;
using vision::ColorRasterFrame;
using vision::OcrRecognitionReuse;
using vision::PpOcrDetection;
using vision::PpOcrTextBox;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Exception>
void require_throws(const std::function<void()>& action, const std::string& message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  }
  require(false, message);
}

// A 1280x720 OCR frame (analysis factor 2): light-grey background with a
// faint diagonal texture, so frames are not flat.
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

ColorRasterFrame blank_frame() {
  ColorRasterFrame frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto shade = static_cast<std::uint8_t>(200 + (x + y) % 8);
      frame.pixels[static_cast<std::size_t>(y) * kWidth + x] = {shade, shade, shade};
    }
  }
  return frame;
}

void paint(ColorRasterFrame& frame, int left, int top, int right, int bottom,
           std::uint8_t value) {
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      frame.pixels[static_cast<std::size_t>(y) * frame.width + x] = {value, value, value};
    }
  }
}

// The sign: a white panel with dark "letters" (vertical strokes).
const PpOcrTextBox kSign{.left = 200, .top = 100, .right = 600, .bottom = 160};

void draw_sign(ColorRasterFrame& frame) {
  paint(frame, kSign.left, kSign.top, kSign.right, kSign.bottom, 250);
  for (int x = kSign.left + 10; x + 6 < kSign.right; x += 24) {
    paint(frame, x, kSign.top + 10, x + 6, kSign.bottom - 10, 20);
  }
}

ColorRasterFrame sign_frame() {
  ColorRasterFrame frame = blank_frame();
  draw_sign(frame);
  return frame;
}

PpOcrTextBox shifted(PpOcrTextBox box, int dx, int dy) {
  box.left += dx;
  box.right += dx;
  box.top += dy;
  box.bottom += dy;
  return box;
}

// A recognizer that reads "s<ordinal>#<box index>" and counts its boxes.
struct FakeRecognizer {
  std::uint64_t ordinal = 0;
  std::size_t calls = 0;
  std::size_t boxes = 0;
  std::vector<std::string> empty_texts;  // texts it reads as nothing

  vision::PpOcrRecognizer bind() {
    return [this](std::span<const PpOcrTextBox> some) {
      ++calls;
      boxes += some.size();
      std::vector<PpOcrDetection> readings;
      for (std::size_t index = 0; index < some.size(); ++index) {
        PpOcrDetection reading;
        reading.text = "s" + std::to_string(ordinal) + "#" + std::to_string(index);
        for (const std::string& empty : empty_texts) {
          if (empty == reading.text) reading.text.clear();
        }
        if (!reading.text.empty()) {
          reading.score = 0.75;
          reading.bbox_left = std::max(0, some[index].left);
          reading.bbox_top = std::max(0, some[index].top);
          reading.bbox_right = std::min(kWidth, some[index].right);
          reading.bbox_bottom = std::min(kHeight, some[index].bottom);
        }
        readings.push_back(reading);
      }
      return readings;
    };
  }
};

std::vector<PpOcrDetection> read(OcrRecognitionReuse& reuse, FakeRecognizer& recognizer,
                                 std::uint64_t ordinal, const ColorRasterFrame& frame,
                                 std::vector<PpOcrTextBox> boxes) {
  recognizer.ordinal = ordinal;
  return reuse.read(ordinal, frame, boxes, recognizer.bind());
}

// --- analysis frame --------------------------------------------------------

void test_analysis_frame_size() {
  const auto size = [](int width, int height) {
    ColorRasterFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.assign(static_cast<std::size_t>(width) * height, {0, 0, 0});
    const vision::OcrReuseAnalysisFrame analysis = vision::make_ocr_reuse_analysis_frame(frame);
    return std::to_string(analysis.width) + "x" + std::to_string(analysis.height) + "/" +
           std::to_string(analysis.factor);
  };
  require(size(1920, 1080) == "640x360/3", "1080p analyses at the measured 640x360");
  require(size(1280, 720) == "640x360/2", "720p analyses at 640x360");
  require(size(640, 360) == "640x360/1", "a small frame is analysed as is");
  require(size(1080, 1920) == "360x640/3", "portrait keeps its orientation");

  ColorRasterFrame grey = blank_frame();
  paint(grey, 0, 0, 2, 2, 100);
  const auto analysis = vision::make_ocr_reuse_analysis_frame(grey);
  require(analysis.luma[0] == 100, "a block averages its pixels' luma");
}

// --- reuse rule ------------------------------------------------------------

void test_unchanged_box_reuses() {
  OcrRecognitionReuse reuse;
  FakeRecognizer recognizer;
  const ColorRasterFrame frame = sign_frame();
  const auto first = read(reuse, recognizer, 0, frame, {kSign});
  require(first.size() == 1 && first[0].text == "s0#0" && !first[0].reused,
          "the first sample of a group recognizes");

  // Detector jitter of a few pixels still matches (IoU > 0.5).
  const PpOcrTextBox jittered = shifted(kSign, 3, -2);
  const auto second = read(reuse, recognizer, 1, frame, {jittered});
  require(recognizer.calls == 1, "an all-reused sample does not call the recognizer");
  require(second.size() == 1 && second[0].text == "s0#0" && second[0].reused &&
              second[0].score == first[0].score,
          "the unchanged box reuses text and score");
  require(second[0].bbox_left == jittered.left && second[0].bbox_top == jittered.top &&
              second[0].bbox_right == jittered.right && second[0].bbox_bottom == jittered.bottom,
          "a reused reading carries the box detected on its own frame");
  require(reuse.recognized_boxes() == 1 && reuse.reused_boxes() == 1, "counters");

  // A change outside the box does not matter.
  ColorRasterFrame elsewhere = frame;
  paint(elsewhere, 700, 400, 1000, 600, 0);
  const auto third = read(reuse, recognizer, 2, elsewhere, {kSign});
  require(third[0].reused && third[0].text == "s0#0", "changes outside the box are ignored");
}

void test_changed_box_is_recognized_again() {
  OcrRecognitionReuse reuse;
  FakeRecognizer recognizer;
  (void)read(reuse, recognizer, 0, sign_frame(), {kSign});

  // New text written inside the box.
  ColorRasterFrame written = sign_frame();
  paint(written, kSign.left + 30, kSign.top + 20, kSign.left + 120, kSign.top + 40, 20);
  const auto changed = read(reuse, recognizer, 1, written, {kSign});
  require(changed[0].text == "s1#0" && !changed[0].reused, "a changed box is recognized again");

  // Below the threshold: a speck under 0.5% of the box's analysis pixels.
  OcrRecognitionReuse quiet;
  (void)read(quiet, recognizer, 0, sign_frame(), {kSign});
  ColorRasterFrame speck = sign_frame();
  paint(speck, kSign.left + 30, kSign.top + 20, kSign.left + 34, kSign.top + 26, 20);
  const auto kept = read(quiet, recognizer, 1, speck, {kSign});
  require(kept[0].reused, "a change in under 0.5% of the box keeps the reading");

  // Small changes accumulate against the frame the text was recognized on,
  // not against the previous sample.
  // The speck is 6 of the box's 6000 analysis pixels; this patch adds 25, so
  // 31 differ from the recognized frame (over 0.5%) but only 25 from the
  // previous sample.
  ColorRasterFrame more = speck;
  paint(more, 484, 120, 494, 130, 20);
  const auto drifted = read(quiet, recognizer, 2, more, {kSign});
  require(!drifted[0].reused && drifted[0].text == "s2#0",
          "drift is measured from the last recognition");
}

void test_moved_and_new_boxes_are_recognized() {
  OcrRecognitionReuse reuse;
  FakeRecognizer recognizer;
  (void)read(reuse, recognizer, 0, sign_frame(), {kSign});
  // Same pixels under a box that barely overlaps the old one.
  ColorRasterFrame moved = blank_frame();
  draw_sign(moved);
  const PpOcrTextBox far = shifted(kSign, 250, 0);
  const auto readings = read(reuse, recognizer, 1, moved, {kSign, far});
  require(readings.size() == 2, "one reading per box");
  require(readings[0].reused && readings[0].text == "s0#0", "the matched box reuses");
  require(!readings[1].reused && readings[1].text == "s1#0",
          "a box without a match (IoU <= 0.5) is recognized; readings keep box order");
  require(recognizer.boxes == 2, "only the unmatched box went to the recognizer");
}

void test_empty_reading_is_reused_as_empty() {
  OcrRecognitionReuse reuse;
  FakeRecognizer recognizer;
  recognizer.empty_texts = {"s0#0"};
  const auto first = read(reuse, recognizer, 0, sign_frame(), {kSign});
  require(first[0].text.empty(), "the recognizer read nothing");
  const auto second = read(reuse, recognizer, 1, sign_frame(), {kSign});
  require(second[0].reused && second[0].text.empty() && second[0].bbox_right == 0,
          "an empty reading is reused as empty, so it is dropped again");
}

void test_reread_cap_and_groups() {
  OcrRecognitionReuse reuse;
  FakeRecognizer recognizer;
  const ColorRasterFrame frame = sign_frame();
  std::string pattern;
  for (std::uint64_t ordinal = 0; ordinal < 8; ++ordinal) {
    const auto readings = read(reuse, recognizer, ordinal, frame, {kSign});
    pattern += readings[0].reused ? "U" : "R";
  }
  require(pattern == "RUURUURU", "an unchanged box is re-read after " +
                                     std::to_string(vision::kOcrReuseMaxConsecutive) +
                                     " reuses, at each group start: " + pattern);
  require(vision::kOcrReuseGroupSamples ==
              static_cast<std::uint64_t>(vision::kOcrReuseMaxConsecutive) + 1,
          "a group is one recognition plus the capped reuses");

  // A missing sample (decode miss) breaks the chain.
  OcrRecognitionReuse gap;
  (void)read(gap, recognizer, 3, frame, {kSign});
  const auto after_gap = read(gap, recognizer, 5, frame, {kSign});
  require(!after_gap[0].reused, "a sample after a gap recognizes everything");
  const auto next = read(gap, recognizer, 6, frame, {kSign});
  require(!next[0].reused, "a new group recognizes everything");
}

// --- batching --------------------------------------------------------------

// Ten samples of a scene: a sign that stays, gets new text, then moves, plus
// a second sign that appears midway.
std::vector<std::pair<ColorRasterFrame, std::vector<PpOcrTextBox>>> scene() {
  std::vector<std::pair<ColorRasterFrame, std::vector<PpOcrTextBox>>> samples;
  const PpOcrTextBox second{.left = 700, .top = 500, .right = 1000, .bottom = 560};
  for (int index = 0; index < 10; ++index) {
    ColorRasterFrame frame = sign_frame();
    std::vector<PpOcrTextBox> boxes = {shifted(kSign, index % 2, 0)};
    if (index >= 4) {
      paint(frame, kSign.left + 30, kSign.top + 20, kSign.left + 120, kSign.top + 40, 20);
    }
    if (index >= 5) {
      paint(frame, second.left, second.top, second.right, second.bottom, 240);
      boxes.push_back(second);
    }
    if (index >= 8) boxes[0] = shifted(kSign, 220, 0);
    samples.emplace_back(std::move(frame), std::move(boxes));
  }
  return samples;
}

std::vector<std::vector<PpOcrDetection>> read_in_batches(
    const std::vector<vision::OcrSampleBatch>& batches) {
  const auto samples = scene();
  std::vector<std::vector<PpOcrDetection>> readings(samples.size());
  FakeRecognizer recognizer;
  for (const vision::OcrSampleBatch& batch : batches) {
    OcrRecognitionReuse reuse;  // one per batch, as run_ocr_frame_batch has
    for (std::uint64_t ordinal = batch.first_ordinal;
         ordinal < batch.first_ordinal + batch.count; ++ordinal) {
      readings[ordinal] =
          read(reuse, recognizer, ordinal, samples[ordinal].first, samples[ordinal].second);
    }
  }
  return readings;
}

bool same_readings(const std::vector<std::vector<PpOcrDetection>>& left,
                   const std::vector<std::vector<PpOcrDetection>>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t sample = 0; sample < left.size(); ++sample) {
    if (left[sample].size() != right[sample].size()) return false;
    for (std::size_t box = 0; box < left[sample].size(); ++box) {
      const PpOcrDetection& a = left[sample][box];
      const PpOcrDetection& b = right[sample][box];
      if (a.text != b.text || a.score != b.score || a.reused != b.reused ||
          a.bbox_left != b.bbox_left || a.bbox_top != b.bbox_top ||
          a.bbox_right != b.bbox_right || a.bbox_bottom != b.bbox_bottom) {
        return false;
      }
    }
  }
  return true;
}

void test_batching_does_not_change_readings() {
  constexpr std::size_t kSamples = 10;
  const auto reference = read_in_batches({{.first_ordinal = 0, .count = kSamples}});
  std::size_t reused = 0;
  for (const auto& sample : reference) {
    for (const auto& reading : sample) reused += reading.reused ? 1 : 0;
  }
  require(reused > 0, "the scene exercises reuse");

  // Every batch size a policy can produce, from one group to everything.
  for (const double seconds_per_sample : {10.0, 3.0, 1.6, 1.2, 0.9, 0.6, 0.1}) {
    const vision::OcrBatchPolicy policy{.target_task_seconds = 10.0,
                                        .estimated_seconds_per_sample = seconds_per_sample};
    const auto batches = vision::partition_ocr_samples(kSamples, policy);
    for (const vision::OcrSampleBatch& batch : batches) {
      require(vision::ocr_reuse_group_starts_at(batch.first_ordinal),
              "every batch starts a reuse group");
    }
    require(same_readings(read_in_batches(batches), reference),
            "batches of " + std::to_string(vision::ocr_batch_sample_count(policy)) +
                " read exactly what one batch reads");
  }

  // A batch cut inside a group would not; the runner refuses one.
  vision::PpOcrSession session;
  vision::OcrFrameBatchRequest request;
  request.samples = {{.ordinal = 4, .timestamp_us = 4'000'000}};
  require_throws<std::invalid_argument>(
      [&] { (void)vision::run_ocr_frame_batch(session, {}, request); },
      "a batch starting inside a reuse group is rejected");
}

// --- reconciliation --------------------------------------------------------

internal::FrameDetection sighting(int frame, double confidence, bool reused) {
  internal::FrameDetection detection;
  detection.frame_id = "frame_" + std::to_string(frame);
  detection.timestamp_us = frame * 1'000'000;
  detection.frame_index = static_cast<std::size_t>(frame);
  detection.frame_width = kWidth;
  detection.frame_height = kHeight;
  detection.raw_text = "OPEN HOUSE";
  detection.confidence = confidence;
  detection.bbox_left = kSign.left;
  detection.bbox_top = kSign.top;
  detection.bbox_right = kSign.right;
  detection.bbox_bottom = kSign.bottom;
  detection.reused = reused;
  return detection;
}

void test_reconciliation_counts_sightings_not_copies() {
  const auto reconciled = internal::reconcile_detections(
      {sighting(0, 0.6, false), sighting(1, 0.6, true), sighting(2, 0.6, true),
       sighting(3, 0.8, false)},
      4);
  require(reconciled.size() == 1, "one observation");
  const internal::ReconciledObservation& observation = reconciled[0];
  require(observation.detection_count == 4 && observation.source_frame_ids.size() == 4,
          "reused sightings count as detections on their frames");
  require(observation.start_us == 0 && observation.end_us == 3'000'000,
          "reused sightings extend the time span");
  require(std::abs(observation.confidence - (0.7 + 0.1)) < 1e-9,
          "confidence averages the two recognitions and adds one repeat bonus");

  const auto copies_only = internal::reconcile_detections(
      {sighting(1, 0.6, true), sighting(2, 0.6, true)}, 4);
  require(copies_only.size() == 1 && std::abs(copies_only[0].confidence - 0.6) < 1e-9,
          "copies of one recognition stay at that recognition's confidence");
}

}  // namespace

int main() {
  test_analysis_frame_size();
  test_unchanged_box_reuses();
  test_changed_box_is_recognized_again();
  test_moved_and_new_boxes_are_recognized();
  test_empty_reading_is_reused_as_empty();
  test_reread_cap_and_groups();
  test_batching_does_not_change_readings();
  test_reconciliation_counts_sightings_not_copies();
  std::cout << "All OCR recognition reuse tests passed.\n";
  return 0;
}
