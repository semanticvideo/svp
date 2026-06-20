#include "svp/vision/depth_generation.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_depth_tests";
  std::filesystem::create_directories(tmp_dir);

  // Test 1: Missing model bundle — depth should remain blocked.
  {
    svp::vision::DepthGenerationOptions opts;
    opts.model_cache_root = tmp_dir / "nonexistent_cache";
    opts.model_id = "model_depth_anything_v2_small";

    svp::vision::DepthGenerationResult result =
        svp::vision::generate_depth_blocks(opts, tmp_dir / "test_missing_model");

    require(!result.depth_generation_run,
            "depth generation must not run when model is missing");
    require(!result.depth_blocks_written,
            "depth blocks must not be written when model is missing");
    require(!result.depth_model_available,
            "depth_model_available must be false when model is missing");
    require(!result.blocker.empty(),
            "blocker must be set when model is missing");

    const nlohmann::json json =
        svp::vision::depth_generation_result_to_json(result);
    require(json.at("depth_model_available") == false,
            "JSON: depth_model_available must be false");
    require(json.at("depth_model_verified") == false,
            "JSON: depth_model_verified must be false");
    require(json.at("depth_frame_input_available") == false,
            "JSON: depth_frame_input_available must be false when no frames provided");
    require(json.at("depth_generation_run") == false,
            "JSON: depth_generation_run must be false");
    require(!json.at("blocker").get<std::string>().empty(),
            "JSON: blocker must be non-empty when model is missing");
  }

  // Test 2: Frame input exists but model is missing — depth_frame_input_available
  // must be reported honestly as true, even though depth generation is blocked.
  {
    svp::vision::DepthGenerationOptions opts;
    opts.model_cache_root = tmp_dir / "nonexistent_cache";
    opts.model_id = "model_depth_anything_v2_small";
    // Simulate successfully decoded frames
    opts.frame_input.decoding_attempted = true;
    opts.frame_input.decoding_succeeded = true;
    opts.frame_input.frames_decoded = 2;
    opts.frame_input.frames.resize(2);
    opts.frame_input.frames[0].frame_id = "frame_000001";
    opts.frame_input.frames[0].timestamp_us = 1000000;
    opts.frame_input.frames[0].width = 640;
    opts.frame_input.frames[0].height = 360;
    opts.frame_input.frames[0].keyframe = true;
    opts.frame_input.frames[0].pixels.resize(640 * 360);
    opts.frame_input.frames[1].frame_id = "frame_000002";
    opts.frame_input.frames[1].timestamp_us = 3000000;
    opts.frame_input.frames[1].width = 640;
    opts.frame_input.frames[1].height = 360;
    opts.frame_input.frames[1].keyframe = false;
    opts.frame_input.frames[1].pixels.resize(640 * 360);

    svp::vision::DepthGenerationResult result =
        svp::vision::generate_depth_blocks(opts, tmp_dir / "test_frames_no_model");

    // Frame input is available even though model is missing
    require(result.depth_frame_input_available,
            "depth_frame_input_available must be true when frames are decoded, "
            "even if model is missing");
    require(!result.depth_generation_run,
            "depth generation must not run when model is missing");
    require(!result.depth_blocks_written,
            "depth blocks must not be written when model is missing");
    require(!result.depth_model_available,
            "depth_model_available must be false when model is missing");
    require(!result.blocker.empty(),
            "blocker must be set when model is missing");

    const nlohmann::json json =
        svp::vision::depth_generation_result_to_json(result);
    require(json.at("depth_frame_input_available") == true,
            "JSON: depth_frame_input_available must be true when frames exist");
    require(json.at("depth_generation_run") == false,
            "JSON: depth_generation_run must be false when model is missing");
    require(json.at("depth_blocks_written") == false,
            "JSON: depth_blocks_written must be false when model is missing");
  }

  // Test 3: Frame input failed (decoding attempted but failed)
  {
    svp::vision::DepthGenerationOptions opts;
    opts.model_cache_root = tmp_dir / "nonexistent_cache";
    opts.model_id = "model_depth_anything_v2_small";
    opts.frame_input.decoding_attempted = true;
    opts.frame_input.decoding_succeeded = false;
    opts.frame_input.skipped_reason = "test: all frames missed";

    svp::vision::DepthGenerationResult result =
        svp::vision::generate_depth_blocks(opts, tmp_dir / "test_failed_frames");

    require(!result.depth_model_available,
            "model should not be available with nonexistent cache");
    require(!result.depth_frame_input_available,
            "frame input should not be available when decoding failed");
    require(!result.depth_generation_run,
            "depth generation must not run when decoding failed");
  }

  // Test 4: Verify provenance fields are present in JSON output
  {
    svp::vision::DepthGenerationOptions opts;
    opts.model_cache_root = tmp_dir / "nonexistent_cache";

    svp::vision::DepthGenerationResult result =
        svp::vision::generate_depth_blocks(opts, tmp_dir / "test_provenance");

    const nlohmann::json json =
        svp::vision::depth_generation_result_to_json(result);

    require(json.contains("onnx_runtime_available"),
            "JSON must contain onnx_runtime_available");
    require(json.contains("depth_model_available"),
            "JSON must contain depth_model_available");
    require(json.contains("depth_model_verified"),
            "JSON must contain depth_model_verified");
    require(json.contains("depth_frame_input_available"),
            "JSON must contain depth_frame_input_available");
    require(json.contains("depth_generation_run"),
            "JSON must contain depth_generation_run");
    require(json.contains("depth_blocks_written"),
            "JSON must contain depth_blocks_written");
    require(json.contains("processor_provenance"),
            "JSON must contain processor_provenance");
    require(json.at("processor_provenance").contains("status"),
            "provenance must contain status");
    require(json.at("processor_provenance").contains("note"),
            "provenance must contain note");
  }

  // Test 5: float_depth_to_uint16 per-frame normalization behavior
  {
    // 5a: Finite varied depth range maps to expected 0..65535 relative range.
    //     min (farthest) -> 0, max (nearest) -> 65535.
    {
      std::vector<float> varied = {1.0f, 5.0f, 10.0f, 3.0f};  // min=1, max=10
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(varied, 2, 2);
      require(r.size() == 4, "varied finite depth should produce 4 values");
      require(r[0] == 0,     "min value (1.0) should map to 0 (farthest)");
      require(r[2] == 65535, "max value (10.0) should map to 65535 (nearest)");
      // 5.0 is (5-1)/(10-1) = 4/9 of the range
      const std::uint16_t expected_mid =
          static_cast<std::uint16_t>(std::lround((4.0f / 9.0f) * 65535.0f));
      require(r[1] == expected_mid,
              "5.0 should map to expected normalized value");
    }

    // 5b: NaN present blocks generation (returns empty).
    {
      std::vector<float> with_nan = {1.0f, 5.0f, std::nanf(""), 3.0f};
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(with_nan, 2, 2);
      require(r.empty(), "NaN in depth output must block (return empty)");
    }

    // 5c: Inf present blocks generation (returns empty).
    {
      std::vector<float> with_inf = {1.0f, 5.0f, std::numeric_limits<float>::infinity(), 3.0f};
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(with_inf, 2, 2);
      require(r.empty(), "Inf in depth output must block (return empty)");
    }

    // 5d: All-invalid (all NaN) blocks generation.
    {
      std::vector<float> all_nan = {std::nanf(""), std::nanf(""), std::nanf(""), std::nanf("")};
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(all_nan, 2, 2);
      require(r.empty(), "all-NaN depth output must block (return empty)");
    }

    // 5e: Constant finite output blocks generation (no meaningful depth variation).
    {
      std::vector<float> constant = {3.0f, 3.0f, 3.0f, 3.0f};
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(constant, 2, 2);
      require(r.empty(),
              "constant finite depth output must block (return empty), "
              "not pretend to contain meaningful depth variation");
    }

    // 5f: Size mismatch blocks generation.
    {
      std::vector<float> short_depth = {0.0f, 0.5f};
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(short_depth, 2, 2);
      require(r.empty(), "short ONNX output must return empty, not zero-pad");

      std::vector<float> long_depth = {0.0f, 0.5f, 1.0f, 0.25f, 0.75f};
      r = svp::vision::float_depth_to_uint16(long_depth, 2, 2);
      require(r.empty(), "oversized ONNX output must return empty, not truncate");

      std::vector<float> empty_depth;
      r = svp::vision::float_depth_to_uint16(empty_depth, 2, 2);
      require(r.empty(), "empty ONNX output must return empty");
    }

    // 5g: Negative depth values are handled correctly by per-frame normalization.
    //     The model may output negative floats; normalization maps min->0, max->65535.
    {
      std::vector<float> negatives = {-5.0f, -1.0f, -3.0f, 0.0f};  // min=-5, max=0
      std::vector<std::uint16_t> r =
          svp::vision::float_depth_to_uint16(negatives, 2, 2);
      require(r.size() == 4, "negative finite depth should produce 4 values");
      require(r[0] == 0,     "min value (-5.0) should map to 0 (farthest)");
      require(r[3] == 65535, "max value (0.0) should map to 65535 (nearest)");
    }
  }

  // Cleanup
  std::filesystem::remove_all(tmp_dir);

  std::cout << "depth_generation_boundary_tests: all checks passed\n";
  return 0;
}
