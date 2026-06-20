// Tests for deterministic scene segmentation by dominant color bucket change.
//
// Covers:
// 1. Single frame → one scene, one shot.
// 2. All-same-color frames → one scene.
// 3. Color change produces multiple scenes.
// 4. Short-segment merging with min_scene_frames=2.
// 5. Shot count matches frame count.
// 6. Scene IDs are sequential and unique.
// 7. Empty frames throws.
// 8. Method string is set.

#include "svp/vision/scene_segmentation.hpp"
#include "svp/vision/color_frame_sampling.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

svp::vision::ColorRasterFrame make_frame(
    const std::string& id,
    std::int64_t ts,
    bool keyframe,
    std::vector<svp::vision::Srgb8Pixel> pixels,
    int width = 2,
    int height = 2) {
  return svp::vision::ColorRasterFrame{id, ts, width, height, keyframe,
                                       std::move(pixels)};
}

// All-orange frames → one scene
std::vector<svp::vision::ColorRasterFrame> make_uniform_orange_frames(int n) {
  std::vector<svp::vision::ColorRasterFrame> frames;
  for (int i = 0; i < n; ++i) {
    frames.push_back(make_frame(
        "frame_" + std::to_string(i + 1), i * 1000000, i == 0,
        {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {255, 128, 0}}));
  }
  return frames;
}

// Frames that transition from orange to pink to green to blue
std::vector<svp::vision::ColorRasterFrame> make_color_block_frames() {
  return {
      make_frame("frame_01", 0, true,
                 {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {255, 128, 0}}),
      make_frame("frame_02", 2000000, false,
                 {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {255, 128, 0}}),
      make_frame("frame_03", 4000000, true,
                 {{255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {255, 0, 255}}),
      make_frame("frame_04", 6000000, false,
                 {{255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {255, 0, 255}}),
      make_frame("frame_05", 8000000, true,
                 {{0, 255, 0}, {0, 255, 0}, {0, 255, 0}, {0, 255, 0}}),
      make_frame("frame_06", 10000000, false,
                 {{0, 255, 0}, {0, 255, 0}, {0, 255, 0}, {0, 255, 0}}),
      make_frame("frame_07", 12000000, true,
                 {{0, 0, 255}, {0, 0, 255}, {0, 0, 255}, {0, 0, 255}}),
      make_frame("frame_08", 14000000, false,
                 {{0, 0, 255}, {0, 0, 255}, {0, 0, 255}, {0, 0, 255}}),
  };
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  // Test 1: single frame
  // -------------------------------------------------------------------------
  {
    auto frames = make_uniform_orange_frames(1);
    auto result = svp::vision::segment_frames_by_color_change(frames);

    require(result.scenes.size() == 1, "single frame: one scene");
    require(result.shots.size() == 1, "single frame: one shot");
    require(result.scenes[0].frame_ids.size() == 1,
            "single frame: scene has one frame");
    require(result.scenes[0].dominant_bucket == "orange",
            "single frame: dominant bucket is orange");
    require(!result.method.empty(), "single frame: method is set");
  }

  // -------------------------------------------------------------------------
  // Test 2: all same color → one scene
  // -------------------------------------------------------------------------
  {
    auto frames = make_uniform_orange_frames(5);
    auto result = svp::vision::segment_frames_by_color_change(frames);

    require(result.scenes.size() == 1, "uniform: one scene");
    require(result.shots.size() == 5, "uniform: 5 shots (one per frame)");
    require(result.scenes[0].frame_ids.size() == 5,
            "uniform: scene spans all 5 frames");
    require(result.scenes[0].dominant_bucket == "orange",
            "uniform: dominant bucket is orange");
  }

  // -------------------------------------------------------------------------
  // Test 3: color changes produce multiple scenes
  // -------------------------------------------------------------------------
  {
    auto frames = make_color_block_frames();
    auto result = svp::vision::segment_frames_by_color_change(frames, 1);

    require(result.scenes.size() == 4,
            "color blocks: 4 scenes (orange, pink, green, blue)");
    require(result.shots.size() == 8,
            "color blocks: 8 shots (one per frame)");

    require(result.scenes[0].dominant_bucket == "orange",
            "color blocks: scene 0 is orange");
    require(result.scenes[1].dominant_bucket == "pink",
            "color blocks: scene 1 is pink");
    require(result.scenes[2].dominant_bucket == "green",
            "color blocks: scene 2 is green");
    require(result.scenes[3].dominant_bucket == "blue",
            "color blocks: scene 3 is blue");

    // Check scene time spans
    require(result.scenes[0].start_us == 0,
            "color blocks: scene 0 starts at 0");
    require(result.scenes[0].end_us == 2000000,
            "color blocks: scene 0 ends at 2s");
    require(result.scenes[3].start_us == 12000000,
            "color blocks: scene 3 starts at 12s");
    require(result.scenes[3].end_us == 14000000,
            "color blocks: scene 3 ends at 14s");
  }

  // -------------------------------------------------------------------------
  // Test 4: short-segment merging with min_scene_frames=2
  // -------------------------------------------------------------------------
  {
    // Create frames: orange, orange, pink (1 frame), green, green
    // With min_scene_frames=2, the single pink frame should merge into
    // the previous segment (orange).
    std::vector<svp::vision::ColorRasterFrame> frames = {
        make_frame("f1", 0, true,
                   {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {255, 128, 0}}),
        make_frame("f2", 1000000, false,
                   {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {255, 128, 0}}),
        make_frame("f3", 2000000, true,
                   {{255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {255, 0, 255}}),
        make_frame("f4", 3000000, false,
                   {{0, 255, 0}, {0, 255, 0}, {0, 255, 0}, {0, 255, 0}}),
        make_frame("f5", 4000000, true,
                   {{0, 255, 0}, {0, 255, 0}, {0, 255, 0}, {0, 255, 0}}),
    };

    auto result = svp::vision::segment_frames_by_color_change(frames, 2);

    // Without merging: orange(2), pink(1), green(2) → 3 segments
    // With merging (min=2): pink(1) merges into orange → orange(3), green(2) → 2 segments
    require(result.scenes.size() == 2,
            "merge: 2 scenes after merging short pink segment");
    require(result.scenes[0].frame_ids.size() == 3,
            "merge: scene 0 has 3 frames (orange+pink merged)");
    require(result.scenes[1].frame_ids.size() == 2,
            "merge: scene 1 has 2 frames (green)");
  }

  // -------------------------------------------------------------------------
  // Test 5: shot IDs are sequential and unique
  // -------------------------------------------------------------------------
  {
    auto frames = make_color_block_frames();
    auto result = svp::vision::segment_frames_by_color_change(frames, 1);

    for (std::size_t i = 0; i < result.shots.size(); ++i) {
      const std::string expected_id =
          "shot_" + std::string(6 - std::to_string(i + 1).size(), '0') +
          std::to_string(i + 1);
      require(result.shots[i].target_id == expected_id,
              "shot IDs: shot " + std::to_string(i) + " has correct id");
    }

    // Each shot has exactly one frame
    for (const auto& shot : result.shots) {
      require(shot.frame_ids.size() == 1,
              "shots: each shot has exactly one frame");
    }
  }

  // -------------------------------------------------------------------------
  // Test 6: scene IDs are sequential and unique
  // -------------------------------------------------------------------------
  {
    auto frames = make_color_block_frames();
    auto result = svp::vision::segment_frames_by_color_change(frames, 1);

    require(result.scenes[0].scene_id == "scene_000001",
            "scene IDs: first is scene_000001");
    require(result.scenes[1].scene_id == "scene_000002",
            "scene IDs: second is scene_000002");
    require(result.scenes[2].scene_id == "scene_000003",
            "scene IDs: third is scene_000003");
    require(result.scenes[3].scene_id == "scene_000004",
            "scene IDs: fourth is scene_000004");
  }

  // -------------------------------------------------------------------------
  // Test 7: empty frames throws
  // -------------------------------------------------------------------------
  {
    bool threw = false;
    try {
      svp::vision::segment_frames_by_color_change({});
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    require(threw, "empty frames: throws invalid_argument");
  }

  // -------------------------------------------------------------------------
  // Test 8: method string is set correctly
  // -------------------------------------------------------------------------
  {
    auto frames = make_uniform_orange_frames(3);
    auto result = svp::vision::segment_frames_by_color_change(frames);
    require(result.method == "deterministic_color_distribution_change_v2",
            "method: correct method string");
  }

  // -------------------------------------------------------------------------
  // Test 9: mixed-red vs near-solid-red split (same dominant bucket)
  //
  // Group A: mixed red/pink/purple/gray — red is dominant but only ~25%.
  // Group B: near-solid red — red is ~100%.
  // Both have dominant bucket "red" but the L1 distance and coverage jump
  // should force a boundary.
  // -------------------------------------------------------------------------
  {
    // 4x4 frames to get enough pixel diversity.
    // Group A: 5 red, 3 pink, 3 purple, 3 gray, 2 brown = 16 pixels.
    //   Red ~31%, pink ~19%, purple ~19%, gray ~19%, brown ~12%.
    //   Dominant = red, diversity = 4 (red, pink, purple, brown).
    std::vector<svp::vision::Srgb8Pixel> mixed_pixels = {
        // 5 red (hue ~23, high chroma)
        {240, 10, 50}, {240, 10, 50}, {240, 10, 50},
        {240, 10, 50}, {240, 10, 50},
        // 3 pink (hue ~344, high chroma)
        {255, 100, 200}, {255, 100, 200}, {255, 100, 200},
        // 3 purple (hue ~300, high chroma)
        {120, 40, 200}, {120, 40, 200}, {120, 40, 200},
        // 3 gray (low chroma)
        {128, 128, 128}, {128, 128, 128}, {128, 128, 128},
        // 2 brown (hue ~62, moderate chroma, low lightness)
        {100, 60, 20}, {100, 60, 20},
    };

    // Group B: 16 red pixels — near-solid red.
    std::vector<svp::vision::Srgb8Pixel> solid_red_pixels(
        16, {240, 10, 50});

    std::vector<svp::vision::ColorRasterFrame> frames = {
        make_frame("mr_1", 0, true, mixed_pixels, 4, 4),
        make_frame("mr_2", 1000000, false, mixed_pixels, 4, 4),
        make_frame("mr_3", 2000000, true, mixed_pixels, 4, 4),
        make_frame("sr_1", 3000000, true, solid_red_pixels, 4, 4),
        make_frame("sr_2", 4000000, false, solid_red_pixels, 4, 4),
        make_frame("sr_3", 5000000, true, solid_red_pixels, 4, 4),
    };

    auto result = svp::vision::segment_frames_by_color_change(frames, 1);

    require(result.scenes.size() == 2,
            "mixed-red vs solid-red: should split into 2 scenes");
    require(result.scenes[0].dominant_bucket == "red",
            "mixed-red scene: dominant bucket is red");
    require(result.scenes[1].dominant_bucket == "red",
            "solid-red scene: dominant bucket is red");
    require(result.scenes[0].frame_ids.size() == 3,
            "mixed-red scene: 3 frames");
    require(result.scenes[1].frame_ids.size() == 3,
            "solid-red scene: 3 frames");
  }

  // -------------------------------------------------------------------------
  // Test 10: gray/warm vs gray/multicolor split (same dominant bucket)
  //
  // Group A: gray-dominant warm scene — gray ~60%, orange ~20%, brown ~20%.
  //   Diversity = 2 (orange, brown).
  // Group B: gray-dominant multicolor — gray ~50%, yellow ~12%, purple ~12%,
  //   red ~12%, green ~14%.
  //   Diversity = 4 (yellow, purple, red, green).
  // Same dominant bucket "gray" but diversity change >= 2 forces a split.
  // -------------------------------------------------------------------------
  {
    // 4x4 = 16 pixels.
    // Group A: 10 gray, 3 orange, 3 brown.
    std::vector<svp::vision::Srgb8Pixel> warm_gray_pixels = {
        {128, 128, 128}, {128, 128, 128}, {128, 128, 128}, {128, 128, 128},
        {128, 128, 128}, {128, 128, 128}, {128, 128, 128}, {128, 128, 128},
        {128, 128, 128}, {128, 128, 128},
        {255, 140, 0}, {255, 140, 0}, {255, 140, 0},
        {100, 60, 20}, {100, 60, 20}, {100, 60, 20},
    };

    // Group B: 8 gray, 2 yellow, 2 purple, 2 red, 2 green.
    std::vector<svp::vision::Srgb8Pixel> multicolor_gray_pixels = {
        {128, 128, 128}, {128, 128, 128}, {128, 128, 128}, {128, 128, 128},
        {128, 128, 128}, {128, 128, 128}, {128, 128, 128}, {128, 128, 128},
        {230, 210, 30}, {230, 210, 30},
        {140, 60, 200}, {140, 60, 200},
        {200, 30, 30}, {200, 30, 30},
        {30, 180, 30}, {30, 180, 30},
    };

    std::vector<svp::vision::ColorRasterFrame> frames = {
        make_frame("wg_1", 0, true, warm_gray_pixels, 4, 4),
        make_frame("wg_2", 1000000, false, warm_gray_pixels, 4, 4),
        make_frame("wg_3", 2000000, true, warm_gray_pixels, 4, 4),
        make_frame("mg_1", 3000000, true, multicolor_gray_pixels, 4, 4),
        make_frame("mg_2", 4000000, false, multicolor_gray_pixels, 4, 4),
        make_frame("mg_3", 5000000, true, multicolor_gray_pixels, 4, 4),
    };

    auto result = svp::vision::segment_frames_by_color_change(frames, 1);

    require(result.scenes.size() == 2,
            "gray/warm vs gray/multicolor: should split into 2 scenes");
    require(result.scenes[0].dominant_bucket == "gray",
            "warm-gray scene: dominant bucket is gray");
    require(result.scenes[1].dominant_bucket == "gray",
            "multicolor-gray scene: dominant bucket is gray");
    require(result.scenes[0].frame_ids.size() == 3,
            "warm-gray scene: 3 frames");
    require(result.scenes[1].frame_ids.size() == 3,
            "multicolor-gray scene: 3 frames");
  }

  // -------------------------------------------------------------------------
  // Test 11: green and blue blocks remain stable (regression)
  //
  // Solid green frames followed by solid blue frames should still produce
  // exactly 2 scenes with the correct dominant buckets.
  // -------------------------------------------------------------------------
  {
    std::vector<svp::vision::Srgb8Pixel> green_pixels(4, {0, 200, 0});
    std::vector<svp::vision::Srgb8Pixel> blue_pixels(4, {0, 0, 200});

    std::vector<svp::vision::ColorRasterFrame> frames = {
        make_frame("g1", 0, true, green_pixels),
        make_frame("g2", 1000000, false, green_pixels),
        make_frame("g3", 2000000, true, green_pixels),
        make_frame("b1", 3000000, true, blue_pixels),
        make_frame("b2", 4000000, false, blue_pixels),
        make_frame("b3", 5000000, true, blue_pixels),
    };

    auto result = svp::vision::segment_frames_by_color_change(frames, 1);

    require(result.scenes.size() == 2,
            "green/blue regression: 2 scenes");
    require(result.scenes[0].dominant_bucket == "green",
            "green/blue regression: scene 0 is green");
    require(result.scenes[1].dominant_bucket == "blue",
            "green/blue regression: scene 1 is blue");
    require(result.scenes[0].frame_ids.size() == 3,
            "green/blue regression: green scene has 3 frames");
    require(result.scenes[1].frame_ids.size() == 3,
            "green/blue regression: blue scene has 3 frames");
  }

  std::cout << "All scene_segmentation tests passed.\n";
  return 0;
}
