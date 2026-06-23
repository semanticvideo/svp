#include "svp/vision/visual_entity_tracker.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

// Test 1: RLE round-trip with all-zero mask
static void test_rle_all_zero() {
  const int w = 4, h = 4;
  std::vector<std::uint8_t> mask(w * h, 0);
  auto rle = svp::vision::encode_mask_rle(mask.data(), w, h);
  assert(!rle.empty());
  // All-zero mask: single run of 16 zeros
  // LEB128(16) = 0x10
  assert(rle.size() == 1);
  assert(rle[0] == 0x10);

  auto decoded = svp::vision::decode_mask_rle(rle.data(), rle.size(), w, h);
  assert(decoded.size() == mask.size());
  assert(std::memcmp(decoded.data(), mask.data(), mask.size()) == 0);
  std::cout << "test_rle_all_zero: PASS\n";
}

// Test 2: RLE round-trip with all-one mask
static void test_rle_all_one() {
  const int w = 4, h = 4;
  std::vector<std::uint8_t> mask(w * h, 1);
  auto rle = svp::vision::encode_mask_rle(mask.data(), w, h);
  assert(!rle.empty());
  // All-one: run of 0 zeros, then run of 16 ones
  // LEB128(0) = 0x00, LEB128(16) = 0x10
  assert(rle.size() == 2);
  assert(rle[0] == 0x00);
  assert(rle[1] == 0x10);

  auto decoded = svp::vision::decode_mask_rle(rle.data(), rle.size(), w, h);
  assert(decoded.size() == mask.size());
  assert(std::memcmp(decoded.data(), mask.data(), mask.size()) == 0);
  std::cout << "test_rle_all_one: PASS\n";
}

// Test 3: RLE round-trip with alternating pattern
static void test_rle_alternating() {
  const int w = 8, h = 2;
  std::vector<std::uint8_t> mask(w * h);
  // Row 0: 0,1,0,1,0,1,0,1
  // Row 1: 1,0,1,0,1,0,1,0
  for (int i = 0; i < w * h; ++i) {
    mask[i] = (i % 2 == 0) ? 0 : 1;
  }
  // Row 0 starts with 0, Row 1 starts with 1 (continuing from row 0)
  // Actually scan order is row-major: 0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0
  // Runs: 0(1), 1(1), 0(1), 1(1), 0(1), 1(1), 0(1), 1(1+1=2), 0(1), 1(1), 0(1), 1(1), 0(1), 1(1), 0(1)

  auto rle = svp::vision::encode_mask_rle(mask.data(), w, h);
  assert(!rle.empty());

  auto decoded = svp::vision::decode_mask_rle(rle.data(), rle.size(), w, h);
  assert(decoded.size() == mask.size());
  assert(std::memcmp(decoded.data(), mask.data(), mask.size()) == 0);
  std::cout << "test_rle_alternating: PASS\n";
}

// Test 4: RLE round-trip with larger mask
static void test_rle_large_mask() {
  const int w = 64, h = 64;
  std::vector<std::uint8_t> mask(w * h, 0);
  // Set a rectangular block in the center
  for (int y = 16; y < 48; ++y) {
    for (int x = 16; x < 48; ++x) {
      mask[y * w + x] = 1;
    }
  }

  auto rle = svp::vision::encode_mask_rle(mask.data(), w, h);
  assert(!rle.empty());

  auto decoded = svp::vision::decode_mask_rle(rle.data(), rle.size(), w, h);
  assert(decoded.size() == mask.size());
  assert(std::memcmp(decoded.data(), mask.data(), mask.size()) == 0);
  std::cout << "test_rle_large_mask: PASS\n";
}

// Test 5: RLE with empty mask
static void test_rle_empty() {
  auto rle = svp::vision::encode_mask_rle(nullptr, 0, 0);
  assert(rle.empty());

  auto decoded = svp::vision::decode_mask_rle(nullptr, 0, 4, 4);
  assert(decoded.empty());
  std::cout << "test_rle_empty: PASS\n";
}

// Test 6: RLE with LEB128 multi-byte values
static void test_rle_leb128_multibyte() {
  const int w = 200, h = 200;
  std::vector<std::uint8_t> mask(w * h, 0);
  // Set first 128 pixels to 1 — 128 requires 2 bytes in LEB128
  for (int i = 0; i < 128; ++i) {
    mask[i] = 1;
  }

  auto rle = svp::vision::encode_mask_rle(mask.data(), w, h);
  assert(!rle.empty());

  auto decoded = svp::vision::decode_mask_rle(rle.data(), rle.size(), w, h);
  assert(decoded.size() == mask.size());
  assert(std::memcmp(decoded.data(), mask.data(), mask.size()) == 0);
  std::cout << "test_rle_leb128_multibyte: PASS\n";
}

// Test 7: Degenerate source detection (all identical frames)
static void test_degenerate_source() {
  // Create 3 identical frames
  svp::vision::VisualEntityTrackerOptions opts;
  std::vector<svp::vision::ColorRasterFrame> frames(3);
  for (int i = 0; i < 3; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(i);
    frames[i].timestamp_us = i * 1000000;
    frames[i].width = 4;
    frames[i].height = 4;
    frames[i].keyframe = (i == 0);
    frames[i].pixels.resize(16);
    for (auto& px : frames[i].pixels) {
      px.r = 128;
      px.g = 128;
      px.b = 128;
    }
  }

  auto result = svp::vision::run_visual_entity_tracker(
      frames, {}, {}, {}, {}, opts);

  // Degenerate source should produce empty results
  assert(result.entities.empty());
  assert(result.tracks.empty());
  assert(result.regions.empty());
  assert(!result.limitations_note.empty());
  std::cout << "test_degenerate_source: PASS\n";
}

// Test 8: Empty frames input
static void test_empty_frames() {
  svp::vision::VisualEntityTrackerOptions opts;
  auto result = svp::vision::run_visual_entity_tracker(
      {}, {}, {}, {}, {}, opts);

  assert(result.entities.empty());
  assert(result.tracks.empty());
  assert(result.regions.empty());
  std::cout << "test_empty_frames: PASS\n";
}

// Test 9: Single frame (insufficient for tracking)
static void test_single_frame() {
  svp::vision::VisualEntityTrackerOptions opts;
  std::vector<svp::vision::ColorRasterFrame> frames(1);
  frames[0].frame_id = "frame_0";
  frames[0].timestamp_us = 0;
  frames[0].width = 8;
  frames[0].height = 8;
  frames[0].keyframe = true;
  frames[0].pixels.resize(64);
  for (auto& px : frames[0].pixels) {
    px.r = 100;
    px.g = 50;
    px.b = 200;
  }

  auto result = svp::vision::run_visual_entity_tracker(
      frames, {}, {}, {}, {}, opts);

  assert(result.entities.empty());
  assert(result.tracks.empty());
  std::cout << "test_single_frame: PASS\n";
}

// Test 10: Result provenance fields are populated
static void test_provenance_fields() {
  svp::vision::VisualEntityTrackerOptions opts;
  opts.embedding_model_id = "test_model";

  std::vector<svp::vision::ColorRasterFrame> frames(2);
  for (int i = 0; i < 2; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(i);
    frames[i].timestamp_us = i * 1000000;
    frames[i].width = 8;
    frames[i].height = 8;
    frames[i].keyframe = (i == 0);
    frames[i].pixels.resize(64);
    for (auto& px : frames[i].pixels) {
      px.r = static_cast<std::uint8_t>(i * 50);
      px.g = static_cast<std::uint8_t>(i * 30);
      px.b = static_cast<std::uint8_t>(i * 20);
    }
  }

  auto result = svp::vision::run_visual_entity_tracker(
      frames, {}, {}, {}, {}, opts);

  assert(!result.processor_id.empty());
  assert(!result.opencv_version.empty());
  assert(result.runtime == "onnxruntime");
  assert(result.confidence_calibration_status == "uncalibrated");
  assert(!result.limitations_note.empty());
  assert(result.parameters_json.contains("keyframe_interval_frames"));
  std::cout << "test_provenance_fields: PASS\n";
}

int main() {
  test_rle_all_zero();
  test_rle_all_one();
  test_rle_alternating();
  test_rle_large_mask();
  test_rle_empty();
  test_rle_leb128_multibyte();
  test_degenerate_source();
  test_empty_frames();
  test_single_frame();
  test_provenance_fields();

  std::cout << "All visual entity tracker tests passed.\n";
  return 0;
}
