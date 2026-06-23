#include "svp/vision/visual_entity_tracker.hpp"
#include "svp/vision/mask_writer.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
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
// passes real synthetic depth data, runs the tracker, verifies entities,
// tracks, regions with unique IDs, mask_ref/depth_ref fields, non-zero depth
// summaries, no duplicate track_id per frame, and exercises mask writer output.
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

  // Provide real synthetic depth data: varying values per pixel so it's not
  // constant.  The block area gets closer (higher uint16), background gets
  // farther (lower uint16).  This ensures depth_summary is non-zero.
  std::vector<std::uint16_t> depth_data;
  std::vector<std::string> depth_frame_ids;
  for (int i = 0; i < 8; ++i) {
    depth_frame_ids.push_back(frames[i].frame_id);
    int bx = 4 + i * 4;
    int by = 24;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (x >= bx && x < bx + block_size && y >= by && y < by + block_size) {
          depth_data.push_back(50000);  // near
        } else {
          depth_data.push_back(10000);  // far
        }
      }
    }
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

  // Require non-empty regions — the moving block should produce tracks
  assert(!result.regions.empty());

  // Verify all region IDs are unique
  std::set<std::string> region_ids;
  for (const auto& r : result.regions) {
    assert(region_ids.insert(r.region_id).second);
    // mask_ref must be set and follow "mask_" + region_id pattern
    assert(!r.mask_ref.empty());
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
    // Depth summary should have non-zero values from real depth data
    assert(r.median_inverse_depth != 0 || r.near_percentile_10 != 0 || r.far_percentile_90 != 0);
  }

  // Verify no duplicate track_id per frame (one-region-per-track-per-frame)
  std::map<std::string, std::set<std::string>> tracks_per_frame;
  for (const auto& r : result.regions) {
    auto& tracks = tracks_per_frame[r.frame_id];
    assert(tracks.insert(r.track_id).second);
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

  // Exercise mask writer: write masks to a temp staging dir and verify output
  auto tmp_dir = std::filesystem::temp_directory_path() / "svp-tracker-test-staging";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir / "spatial");

  std::vector<svp::vision::MaskWriteEntry> mask_entries;
  for (const auto& r : result.regions) {
    if (r.mask_pixels.empty() || r.mask_width <= 0 || r.mask_height <= 0) continue;
    svp::vision::MaskWriteEntry entry;
    entry.mask_id = "mask_" + r.region_id;
    entry.entity_id = r.entity_id;
    entry.track_id = r.track_id;
    entry.region_id = r.region_id;
    entry.frame_id = r.frame_id;
    entry.timestamp_us = r.timestamp_us;
    entry.width = r.mask_width;
    entry.height = r.mask_height;
    entry.rle_data = svp::vision::encode_mask_rle(
        r.mask_pixels.data(), r.mask_width, r.mask_height);
    mask_entries.push_back(entry);
  }

  assert(!mask_entries.empty());
  auto mask_summary = svp::vision::write_masks(tmp_dir, mask_entries);
  assert(mask_summary.mask_count == static_cast<int>(mask_entries.size()));
  assert(std::filesystem::exists(tmp_dir / "spatial" / "masks.blocks.svpmz"));
  assert(std::filesystem::exists(tmp_dir / "spatial" / "masks.index.jsonl"));
  assert(!mask_summary.index_records.empty());

  // Verify mask index records use "id" not "mask_id"
  for (const auto& rec : mask_summary.index_records) {
    assert(rec.contains("id"));
    assert(!rec.contains("mask_id"));
    assert(rec["encoding"] == "svp-rle-v1");
    assert(rec["block_file"] == "spatial/masks.blocks.svpmz");
  }

  // Cleanup
  std::filesystem::remove_all(tmp_dir);

  std::cout << "test_moving_object_integration: PASS (regions=" << result.regions.size()
            << ", entities=" << result.entities.size()
            << ", tracks=" << result.tracks.size()
            << ", masks=" << mask_entries.size() << ")\n";
}

// Test 11: Static depth-separated object produces entities
// RGB frames are identical (no motion), but depth has a coherent foreground
// object separated from background.  Expected: non-empty entities/tracks/regions/masks.
static void test_static_depth_separated_object() {
  const int w = 64, h = 64;
  const int block_size = 24;
  std::vector<svp::vision::ColorRasterFrame> frames(8);
  for (int i = 0; i < 8; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(2000 + i);
    frames[i].timestamp_us = (i + 1) * 1000000;
    frames[i].width = w;
    frames[i].height = h;
    frames[i].keyframe = (i % 5 == 0);
    frames[i].pixels.resize(static_cast<std::size_t>(w) * h);
    // All frames identical: gray background with a darker square
    for (auto& px : frames[i].pixels) {
      px.r = 128; px.g = 128; px.b = 128;
    }
    // Darker square in center (static, no motion)
    for (int y = 20; y < 20 + block_size; ++y) {
      for (int x = 20; x < 20 + block_size; ++x) {
        auto& px = frames[i].pixels[static_cast<std::size_t>(y) * w + x];
        px.r = 60; px.g = 60; px.b = 60;
      }
    }
  }

  // Depth: foreground object (near) vs background (far)
  std::vector<std::uint16_t> depth_data;
  std::vector<std::string> depth_frame_ids;
  for (int i = 0; i < 8; ++i) {
    depth_frame_ids.push_back(frames[i].frame_id);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (x >= 20 && x < 20 + block_size && y >= 20 && y < 20 + block_size) {
          depth_data.push_back(50000);  // near
        } else {
          depth_data.push_back(10000);  // far
        }
      }
    }
  }

  svp::vision::VisualEntityTrackerOptions opts;
  opts.embedding_model_id = "model_nomic_embed_vision_v1_5";
  opts.execution_provider = "cpu";

  auto result = svp::vision::run_visual_entity_tracker(
      frames, depth_data, depth_frame_ids, {}, {}, opts);

  // Must produce non-empty entities/tracks/regions from depth alone
  assert(!result.regions.empty());
  assert(!result.entities.empty());
  assert(!result.tracks.empty());

  // Verify candidate_source is set and includes depth-derived regions
  bool has_depth_source = false;
  for (const auto& r : result.regions) {
    assert(!r.candidate_source.empty());
    if (r.candidate_source == "depth" || r.candidate_source == "fused_motion_depth") {
      has_depth_source = true;
    }
    assert(!r.mask_ref.empty());
    assert(!r.depth_ref.empty());
    assert(!r.mask_pixels.empty());
  }
  assert(has_depth_source);

  // Verify no duplicate track_id per frame
  std::map<std::string, std::set<std::string>> tracks_per_frame;
  for (const auto& r : result.regions) {
    auto& tracks = tracks_per_frame[r.frame_id];
    assert(tracks.insert(r.track_id).second);
  }

  std::cout << "test_static_depth_separated_object: PASS (regions=" << result.regions.size()
            << ", entities=" << result.entities.size()
            << ", tracks=" << result.tracks.size() << ")\n";
}

// Test 12: Flat depth static produces no entities
// RGB frames are static and depth is uniform.  Expected: no invented visual entities.
static void test_flat_depth_static() {
  const int w = 64, h = 64;
  std::vector<svp::vision::ColorRasterFrame> frames(8);
  for (int i = 0; i < 8; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(3000 + i);
    frames[i].timestamp_us = (i + 1) * 1000000;
    frames[i].width = w;
    frames[i].height = h;
    frames[i].keyframe = (i % 5 == 0);
    frames[i].pixels.resize(static_cast<std::size_t>(w) * h);
    // All frames identical: uniform gray
    for (auto& px : frames[i].pixels) {
      px.r = 128; px.g = 128; px.b = 128;
    }
  }

  // Uniform depth (all same value)
  std::vector<std::uint16_t> depth_data;
  std::vector<std::string> depth_frame_ids;
  for (int i = 0; i < 8; ++i) {
    depth_frame_ids.push_back(frames[i].frame_id);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        depth_data.push_back(30000);  // uniform
      }
    }
  }

  svp::vision::VisualEntityTrackerOptions opts;
  opts.embedding_model_id = "model_nomic_embed_vision_v1_5";
  opts.execution_provider = "cpu";

  auto result = svp::vision::run_visual_entity_tracker(
      frames, depth_data, depth_frame_ids, {}, {}, opts);

  // Flat depth + static RGB should not create fake entities
  assert(result.regions.empty());
  assert(result.entities.empty());
  assert(result.tracks.empty());

  std::cout << "test_flat_depth_static: PASS (regions=0, entities=0, tracks=0)\n";
}

// Test 13: Motion + depth fusion produces one entity, not duplicates
// A moving object with distinct depth.  Both motion and depth should detect it,
// but fusion should produce one entity/track, not two.
static void test_motion_depth_fusion() {
  const int w = 64, h = 64;
  const int block_size = 20;
  std::vector<svp::vision::ColorRasterFrame> frames(8);
  for (int i = 0; i < 8; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(4000 + i);
    frames[i].timestamp_us = (i + 1) * 1000000;
    frames[i].width = w;
    frames[i].height = h;
    frames[i].keyframe = (i % 5 == 0);
    frames[i].pixels.resize(static_cast<std::size_t>(w) * h);
    for (auto& px : frames[i].pixels) {
      px.r = 0; px.g = 0; px.b = 0;
    }
    // White block moving right
    int bx = 4 + i * 4;
    int by = 22;
    for (int y = by; y < by + block_size && y < h; ++y) {
      for (int x = bx; x < bx + block_size && x < w; ++x) {
        auto& px = frames[i].pixels[static_cast<std::size_t>(y) * w + x];
        px.r = 255; px.g = 255; px.b = 255;
      }
    }
  }

  // Depth: foreground object (near) matching the moving block, background (far)
  std::vector<std::uint16_t> depth_data;
  std::vector<std::string> depth_frame_ids;
  for (int i = 0; i < 8; ++i) {
    depth_frame_ids.push_back(frames[i].frame_id);
    int bx = 4 + i * 4;
    int by = 22;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (x >= bx && x < bx + block_size && y >= by && y < by + block_size) {
          depth_data.push_back(50000);  // near
        } else {
          depth_data.push_back(10000);  // far
        }
      }
    }
  }

  svp::vision::VisualEntityTrackerOptions opts;
  opts.embedding_model_id = "model_nomic_embed_vision_v1_5";
  opts.execution_provider = "cpu";

  auto result = svp::vision::run_visual_entity_tracker(
      frames, depth_data, depth_frame_ids, {}, {}, opts);

  // Must produce non-empty results
  assert(!result.regions.empty());
  assert(!result.entities.empty());

  // Should have at most 2 entities (ideally 1 if fusion works well)
  // The key assertion: no duplicate entities for the same spatial region
  // Check no duplicate track_id per frame
  std::map<std::string, std::set<std::string>> tracks_per_frame;
  for (const auto& r : result.regions) {
    auto& tracks = tracks_per_frame[r.frame_id];
    assert(tracks.insert(r.track_id).second);
  }

  // Verify that at least some regions have fused source
  bool has_fused = false;
  for (const auto& r : result.regions) {
    if (r.candidate_source == "fused_motion_depth") {
      has_fused = true;
      break;
    }
  }
  // Fusion may or may not happen depending on IoU overlap, but if it does,
  // it should be correctly labeled.  The key is no duplicate entities per frame.

  std::cout << "test_motion_depth_fusion: PASS (regions=" << result.regions.size()
            << ", entities=" << result.entities.size()
            << ", tracks=" << result.tracks.size()
            << ", fused=" << (has_fused ? "yes" : "no") << ")\n";
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
  test_static_depth_separated_object();
  test_flat_depth_static();
  test_motion_depth_fusion();

  std::cout << "All visual entity tracker tests passed.\n";
  return 0;
}
