#include "svp/vision/depth_generation.hpp"
#include "svp/vision/embedding_generation.hpp"
#include "svp/vision/visual_entity_tracker.hpp"
#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/color_quantization.hpp"
#include "svp/vision/evidence_crop.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void test_depth_callback_fires_with_frame_count() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_depth_cb_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  svp::vision::DepthGenerationOptions opts;
  opts.model_cache_root = tmp_dir / "nonexistent_cache";
  opts.model_id = "model_depth_anything_v2_small";

  // Provide 3 synthetic frames so the callback fires with total=3
  opts.frame_input.decoding_succeeded = true;
  opts.frame_input.frames.resize(3);
  for (int i = 0; i < 3; ++i) {
    opts.frame_input.frames[i].frame_id = "frame_" + std::to_string(i);
    opts.frame_input.frames[i].width = 64;
    opts.frame_input.frames[i].height = 64;
    opts.frame_input.frames[i].pixels.resize(64 * 64,
        svp::vision::Srgb8Pixel{128, 128, 128});
  }

  std::size_t call_count = 0;
  std::size_t last_current = 0;
  std::size_t last_total = 0;
  opts.on_progress = [&](std::size_t current, std::size_t total) {
    ++call_count;
    last_current = current;
    last_total = total;
  };

  svp::vision::generate_depth_blocks(opts, tmp_dir / "staging");

  // Callback must have been called at least once with total=3
  assert(call_count > 0);
  assert(last_total == 3);
  assert(last_current <= 3);

  std::cout << "test_depth_callback_fires_with_frame_count: PASS\n";
}

void test_embedding_callback_fires_with_text_count() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_embed_cb_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);
  std::filesystem::create_directories(tmp_dir / "text");

  // Seed 2 text observations
  {
    std::ofstream out(tmp_dir / "text" / "text_observations.jsonl");
    out << R"({"text_observation_id":"text_obs_000001","text_region_id":"text_region_000001","observation_type":"text_recognition","raw_text":"Hello","normalized_text":"hello","confidence":0.9,"provenance_id":"proc_001"})" << "\n";
    out << R"({"text_observation_id":"text_obs_000002","text_region_id":"text_region_000002","observation_type":"text_recognition","raw_text":"World","normalized_text":"world","confidence":0.9,"provenance_id":"proc_001"})" << "\n";
  }

  svp::vision::EmbeddingGenerationOptions opts;
  opts.model_cache_root = tmp_dir / "nonexistent_cache";

  std::size_t call_count = 0;
  std::size_t last_total = 0;
  opts.on_progress = [&](std::size_t current, std::size_t total) {
    ++call_count;
    last_total = total;
  };

  svp::vision::generate_embedding_blocks(opts, tmp_dir);

  // Callback must have been called at least once with total=2
  assert(call_count > 0);
  assert(last_total == 2);

  std::cout << "test_embedding_callback_fires_with_text_count: PASS\n";
}

void test_visual_tracker_tracking_callback_fires() {
  // Create 3 synthetic ColorRasterFrames (64x64) with a white rectangle
  // to create detectable features for Shi-Tomasi corner detection
  std::vector<svp::vision::ColorRasterFrame> frames(3);
  for (int i = 0; i < 3; ++i) {
    frames[i].frame_id = "frame_" + std::to_string(i);
    frames[i].width = 64;
    frames[i].height = 64;
    frames[i].keyframe = (i == 0);
    frames[i].frame_index = static_cast<std::size_t>(i);
    frames[i].pixels.resize(64 * 64,
        svp::vision::Srgb8Pixel{128, 128, 128});
    // Add a white rectangle to create detectable features
    for (int y = 20; y < 40; ++y) {
      for (int x = 20; x < 40; ++x) {
        frames[i].pixels[y * 64 + x] = svp::vision::Srgb8Pixel{255, 255, 255};
      }
    }
  }

  svp::vision::VisualEntityTrackerOptions opts;
  opts.embedding_model_id = "model_nomic_embed_vision_v1_5";
  opts.execution_provider = "cpu";

  std::size_t tracking_call_count = 0;
  std::size_t tracking_last_total = 0;
  opts.on_tracking_progress = [&](std::size_t current, std::size_t total) {
    ++tracking_call_count;
    tracking_last_total = total;
  };

  std::size_t visual_emb_call_count = 0;
  opts.on_visual_embedding_progress = [&](std::size_t current, std::size_t total) {
    ++visual_emb_call_count;
  };

  // No depth data, no shot boundaries, no model cache — should still run
  // optical flow tracking on the synthetic frames
  std::vector<std::uint16_t> depth_data;
  std::vector<std::string> depth_frame_ids;
  std::vector<std::pair<std::string, std::int64_t>> shot_boundaries;

  auto result = svp::vision::run_visual_entity_tracker(
      frames, depth_data, depth_frame_ids, shot_boundaries,
      std::filesystem::path{}, opts);

  // Tracking callback must have been called
  assert(tracking_call_count > 0);
  assert(tracking_last_total >= 1);

  // Visual embedding callback should NOT fire when embedding model is unavailable
  // (no model cache provided)
  assert(visual_emb_call_count == 0);

  std::cout << "test_visual_tracker_tracking_callback_fires: PASS\n";
}

void test_depth_callback_not_called_without_frames() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_depth_cb_empty";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  svp::vision::DepthGenerationOptions opts;
  opts.model_cache_root = tmp_dir / "nonexistent_cache";

  bool called = false;
  opts.on_progress = [&](std::size_t, std::size_t) {
    called = true;
  };

  svp::vision::generate_depth_blocks(opts, tmp_dir / "staging");

  // With no frames, the callback should not fire
  assert(!called);

  std::cout << "test_depth_callback_not_called_without_frames: PASS\n";
}

void test_evidence_crop_callback_counts_skipped_inputs() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_evidence_crop_cb";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  svp::vision::EvidenceCropOptions opts;
  opts.crop_coverage_policy = "one_per_observation";
  opts.ocr_frame_width = 0;
  opts.ocr_frame_height = 0;
  opts.source_frame_width = 0;
  opts.source_frame_height = 0;

  std::vector<svp::vision::CropGenerationInput> inputs(3);
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    inputs[i].text_region_id = "text_region_" + std::to_string(i + 1);
    inputs[i].text_observation_id = "text_obs_" + std::to_string(i + 1);
    inputs[i].source_frame_id = "frame_" + std::to_string(i + 1);
    inputs[i].bbox_left = 0;
    inputs[i].bbox_top = 0;
    inputs[i].bbox_right = 10;
    inputs[i].bbox_bottom = 10;
    inputs[i].frame_width = 10;
    inputs[i].frame_height = 10;
  }

  std::vector<std::pair<std::size_t, std::size_t>> calls;
  opts.on_progress = [&](std::size_t current, std::size_t total) {
    calls.push_back({current, total});
  };

  const svp::vision::EvidenceCropResult result =
      svp::vision::generate_evidence_crops_internal(opts, inputs, tmp_dir);

  assert(calls.size() == inputs.size() + 1);
  assert(calls.front().first == 0);
  assert(calls.front().second == inputs.size());
  assert(calls.back().first == inputs.size());
  assert(calls.back().second == inputs.size());
  assert(result.crop_count == 0);
  assert(result.crops_skipped_count == static_cast<std::int64_t>(inputs.size()));

  std::filesystem::remove_all(tmp_dir);
  std::cout << "test_evidence_crop_callback_counts_skipped_inputs: PASS\n";
}

}  // namespace

int main() {
  test_depth_callback_fires_with_frame_count();
  test_embedding_callback_fires_with_text_count();
  test_visual_tracker_tracking_callback_fires();
  test_depth_callback_not_called_without_frames();
  test_evidence_crop_callback_counts_skipped_inputs();

  std::cout << "All progress callback tests: PASS\n";
  return 0;
}
