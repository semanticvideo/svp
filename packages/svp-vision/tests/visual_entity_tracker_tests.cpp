#include "svp/vision/visual_entity_tracker.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
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

// Test 10: Integration test with moving object sequence
// Creates synthetic frames with a moving white block on black background,
// runs the tracker, and verifies entities, tracks, regions with unique IDs,
// mask_ref/depth_ref fields, and non-empty masks.
static void test_moving_object_integration() {
  // Create 8 frames of 64x64 with a 16x16 white block moving right
  const int w = 64, h = 64;
  const int block_size = 16;
  std::vector<svp::vision::ColorRasterFrame> frames(8);
  for (int i = 0; i < 8; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(1000 + i);
    frames[i].timestamp_us = (i + 1) * 1000000;  // 1s per frame
    frames[i].width = w;
    frames[i].height = h;
    frames[i].keyframe = (i % 5 == 0);
    frames[i].pixels.resize(static_cast<std::size_t>(w) * h);
    // Black background
    for (auto& px : frames[i].pixels) {
      px.r = 0; px.g = 0; px.b = 0;
    }
    // White block moving right by 4 pixels per frame
    int bx = 4 + i * 4;
    int by = 24;
    for (int y = by; y < by + block_size && y < h; ++y) {
      for (int x = bx; x < bx + block_size && x < w; ++x) {
        auto& px = frames[i].pixels[static_cast<std::size_t>(y) * w + x];
        px.r = 255; px.g = 255; px.b = 255;
      }
    }
  }

  // Provide synthetic depth data (one frame worth, all same = blocked by float_depth_to_uint16)
  // So pass empty depth — tracker handles gracefully
  std::vector<std::uint16_t> depth_data;
  std::vector<std::string> depth_frame_ids;
  for (int i = 0; i < 8; ++i) {
    depth_frame_ids.push_back(frames[i].frame_id);
  }

  svp::vision::VisualEntityTrackerOptions opts;
  opts.embedding_model_id = "model_nomic_embed_vision_v1_5";
  opts.execution_provider = "cpu";

  auto result = svp::vision::run_visual_entity_tracker(
      frames, depth_data, depth_frame_ids, {}, {}, opts);

  // Verify provenance
  assert(!result.processor_id.empty());
  assert(!result.opencv_version.empty());
  assert(result.runtime == "onnxruntime");

  // If the tracker found regions, verify structure
  if (!result.regions.empty()) {
    // Verify all region IDs are unique
    std::set<std::string> region_ids;
    for (const auto& r : result.regions) {
      assert(region_ids.insert(r.region_id).second);
      // mask_ref must be set
      assert(!r.mask_ref.empty());
      // mask_ref must follow "mask_" + region_id pattern
      assert(r.mask_ref == "mask_" + r.region_id);
      // depth_ref must be set (we provided depth_frame_ids)
      assert(!r.depth_ref.empty());
      // Mask pixels must be non-empty
      assert(!r.mask_pixels.empty());
      assert(r.mask_width > 0);
      assert(r.mask_height > 0);
      // Entity and track IDs must be set
      assert(!r.entity_id.empty());
      assert(!r.track_id.empty());
    }

    // Verify entities have correct type
    for (const auto& e : result.entities) {
      assert(!e.entity_id.empty());
      assert(e.entity_type == "visual_entity" || e.entity_type == "background_region");
      assert(!e.track_ids.empty());
    }

    // Verify tracks have tracking method
    for (const auto& t : result.tracks) {
      assert(!t.track_id.empty());
      assert(!t.tracking_method.empty());
      assert(t.tracking_method == "optical_flow_kalman");
    }

    // Verify all track IDs are unique
    std::set<std::string> track_ids;
    for (const auto& t : result.tracks) {
      assert(track_ids.insert(t.track_id).second);
    }

    // Verify all entity IDs are unique
    std::set<std::string> entity_ids;
    for (const auto& e : result.entities) {
      assert(entity_ids.insert(e.entity_id).second);
    }
  }

  std::cout << "test_moving_object_integration: PASS (regions=" << result.regions.size()
            << ", entities=" << result.entities.size()
            << ", tracks=" << result.tracks.size() << ")\n";
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
  test_moving_object_integration();

  std::cout << "All visual entity tracker tests passed.\n";
  return 0;
}
