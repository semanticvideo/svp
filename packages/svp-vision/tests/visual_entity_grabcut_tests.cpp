#include "../src/visual_entity_grabcut.hpp"

#include "svp/vision/visual_entity_tracker.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace internal = svp::vision::visual_entity_internal;

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

// Deterministic textured content: GrabCut's k-means++ initialization only
// matters when colours do not separate trivially. Square tiles of unrelated
// colours give many comparable clusterings; the subject carries its own tiles
// so its texture moves with it.
constexpr int kTileSize = 8;

cv::Vec3b tile_colour(std::uint32_t seed, int tile_x, int tile_y) {
  std::uint32_t hash = seed * 2246822519u +
      static_cast<std::uint32_t>(tile_y) * 7919u +
      static_cast<std::uint32_t>(tile_x) * 104729u;
  hash *= 2654435761u;
  return cv::Vec3b(static_cast<std::uint8_t>(hash >> 24),
                   static_cast<std::uint8_t>(hash >> 16),
                   static_cast<std::uint8_t>(hash >> 8));
}

cv::Mat textured_frame(int width, int height, int subject_x, int subject_y,
                       int subject_size, std::uint32_t seed) {
  cv::Mat frame(height, width, CV_8UC3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const bool subject = x >= subject_x && x < subject_x + subject_size &&
                           y >= subject_y && y < subject_y + subject_size;
      frame.at<cv::Vec3b>(y, x) = subject
          ? tile_colour(seed + 1, (x - subject_x) / kTileSize,
                        (y - subject_y) / kTileSize)
          : tile_colour(seed, x / kTileSize, y / kTileSize);
    }
  }
  return frame;
}

// GC_INIT_WITH_RECT initializes the mask from the box.
cv::Mat initial_mask(const cv::Mat& frame) {
  return cv::Mat::zeros(frame.size(), CV_8UC1);
}

bool same_mask(const cv::Mat& left, const cv::Mat& right) {
  return left.size() == right.size() && cv::countNonZero(left != right) == 0;
}

cv::Mat seeded_mask(const cv::Mat& frame, const cv::Rect& box) {
  cv::Mat mask = initial_mask(frame);
  internal::run_seeded_grabcut(frame, mask, box, 5);
  return mask;
}

const cv::Rect kBox(16, 16, 56, 56);

// Guard against a vacuous fixture: plain cv::grabCut on this frame must give
// different masks for different generator states, or the tests below would
// pass without exercising the reseed.
void test_fixture_is_rng_sensitive() {
  const cv::Mat frame = textured_frame(96, 96, 24, 24, 40, 7);
  std::vector<cv::Mat> masks;
  for (std::uint64_t state : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL}) {
    cv::theRNG() = cv::RNG(state);
    cv::Mat mask = initial_mask(frame);
    cv::Mat background_model;
    cv::Mat foreground_model;
    cv::grabCut(frame, mask, kBox, background_model, foreground_model, 5,
                cv::GC_INIT_WITH_RECT);
    masks.push_back(mask);
  }
  bool any_difference = false;
  for (const auto& mask : masks) {
    any_difference = any_difference || !same_mask(masks.front(), mask);
  }
  check(any_difference, "plain GrabCut on the fixture depends on the thread RNG");
}

// A mask depends only on its frame and box: any earlier generator state on the
// calling thread, and any thread, gives the same mask.
void test_mask_is_independent_of_thread_history() {
  const cv::Mat frame = textured_frame(96, 96, 24, 24, 40, 7);
  cv::theRNG() = cv::RNG();
  const cv::Mat reference = seeded_mask(frame, kBox);
  for (std::uint64_t state : {1ULL, 0x1234ULL, 0xdeadbeefcafeULL}) {
    cv::theRNG() = cv::RNG(state);
    check(same_mask(reference, seeded_mask(frame, kBox)),
          "seeded GrabCut ignores the thread's previous RNG state");
  }
  // Earlier GrabCut work on other content must not change a later mask.
  cv::theRNG() = cv::RNG();
  for (std::uint32_t seed = 11; seed < 15; ++seed) {
    (void)seeded_mask(textured_frame(96, 96, 30, 20, 36, seed), kBox);
  }
  check(same_mask(reference, seeded_mask(frame, kBox)),
        "seeded GrabCut ignores earlier GrabCut calls on the thread");
  cv::Mat other_thread_mask;
  std::thread worker([&] {
    cv::theRNG() = cv::RNG(99);
    other_thread_mask = seeded_mask(frame, kBox);
  });
  worker.join();
  check(same_mask(reference, other_thread_mask),
        "seeded GrabCut gives the same mask on another thread");
}

void test_thread_rng_state_is_restored() {
  const cv::Mat frame = textured_frame(96, 96, 24, 24, 40, 7);
  cv::theRNG() = cv::RNG(0xabcdefULL);
  const std::uint64_t before = cv::theRNG().state;
  (void)seeded_mask(frame, kBox);
  check(cv::theRNG().state == before,
        "seeded GrabCut leaves the thread's RNG state unchanged");
}

// A textured subject moving right across a textured background keeps the
// frames non-degenerate. Each frame also has one detector proposal over a
// static textured area away from the subject's path: a detector-only
// candidate is the kind the tracker refines with GrabCut (motion-fused
// candidates keep their motion mask).
constexpr int kFrameSize = 96;
constexpr int kSubjectSize = 28;
constexpr int kSubjectStartX = 8;
constexpr int kSubjectY = 30;
constexpr int kSubjectStepX = 6;
// Below the subject's path (rows kSubjectY .. kSubjectY + kSubjectSize).
const cv::Rect kStaticProposal(8, 64, 48, 30);
constexpr std::int64_t kFrameIntervalUs = 200'000;

int subject_x(int index) { return kSubjectStartX + index * kSubjectStepX; }

std::vector<svp::vision::ColorRasterFrame> moving_textured_frames(
    int frame_count) {
  std::vector<svp::vision::ColorRasterFrame> frames;
  for (int index = 0; index < frame_count; ++index) {
    const cv::Mat image = textured_frame(kFrameSize, kFrameSize,
                                         subject_x(index), kSubjectY,
                                         kSubjectSize, 31);
    svp::vision::ColorRasterFrame frame;
    frame.frame_id = "frame_" + std::to_string(100 + index);
    frame.timestamp_us = (index + 1) * kFrameIntervalUs;
    frame.width = kFrameSize;
    frame.height = kFrameSize;
    frame.keyframe = index == 0;
    frame.pixels.reserve(static_cast<std::size_t>(kFrameSize) * kFrameSize);
    for (int y = 0; y < kFrameSize; ++y) {
      for (int x = 0; x < kFrameSize; ++x) {
        const auto& pixel = image.at<cv::Vec3b>(y, x);
        frame.pixels.push_back({pixel[0], pixel[1], pixel[2]});
      }
    }
    frames.push_back(std::move(frame));
  }
  return frames;
}

std::vector<std::vector<std::uint8_t>> tracker_masks(
    const std::vector<svp::vision::ColorRasterFrame>& frames) {
  svp::vision::VisualEntityTrackerOptions options;
  options.keyframe_interval_frames = 1;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    svp::vision::ExternalEntityProposal proposal;
    proposal.frame_id = frames[index].frame_id;
    proposal.box_px[0] = kStaticProposal.x;
    proposal.box_px[1] = kStaticProposal.y;
    proposal.box_px[2] = kStaticProposal.x + kStaticProposal.width;
    proposal.box_px[3] = kStaticProposal.y + kStaticProposal.height;
    proposal.confidence = 0.9;
    proposal.detector_category_index = 0;
    proposal.source = "objectness_detector";
    options.external_proposals.push_back(proposal);
  }
  const auto result =
      svp::vision::run_visual_entity_tracker(frames, {}, {}, {}, {}, options);
  std::vector<std::vector<std::uint8_t>> masks;
  for (const auto& region : result.regions) masks.push_back(region.mask_pixels);
  return masks;
}

// The tracker's masks for a window do not depend on what ran on the thread
// before it, so a window computed alone reproduces the window computed after
// others.
void test_tracker_window_is_independent_of_thread_history() {
  const auto frames = moving_textured_frames(8);
  cv::theRNG() = cv::RNG();
  const auto reference = tracker_masks(frames);
  check(!reference.empty(), "the detector proposals produce regions");
  cv::theRNG() = cv::RNG(0x5eedULL);
  (void)tracker_masks(moving_textured_frames(5));
  check(tracker_masks(frames) == reference,
        "tracker masks ignore earlier tracking work on the thread");
  std::vector<std::vector<std::uint8_t>> other_thread;
  std::thread worker([&] { other_thread = tracker_masks(frames); });
  worker.join();
  check(other_thread == reference, "tracker masks match on a fresh thread");
}

}  // namespace

int main() {
  test_fixture_is_rng_sensitive();
  test_mask_is_independent_of_thread_history();
  test_thread_rng_state_is_restored();
  test_tracker_window_is_independent_of_thread_history();
  if (failures != 0) return 1;
  std::cout << "All visual entity GrabCut tests passed.\n";
  return 0;
}
