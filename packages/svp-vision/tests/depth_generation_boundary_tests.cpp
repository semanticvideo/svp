#include "svp/vision/depth_generation.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
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

  // Test 5: float_depth_to_uint16 rejects mismatched sizes
  {
    // Correct size: 2x2 = 4 elements
    std::vector<float> correct_depth = {0.0f, 0.5f, 1.0f, 0.25f};
    std::vector<std::uint16_t> result =
        svp::vision::float_depth_to_uint16(correct_depth, 2, 2);
    require(result.size() == 4,
            "correct-size depth output should produce 4 uint16 values");
    require(result[0] == 0,
            "0.0f should map to 0");
    require(result[2] == 65535,
            "1.0f should map to 65535");

    // Too few elements: must return empty, not zero-pad
    std::vector<float> short_depth = {0.0f, 0.5f};
    result = svp::vision::float_depth_to_uint16(short_depth, 2, 2);
    require(result.empty(),
            "short ONNX output must return empty, not zero-pad");

    // Too many elements: must also return empty
    std::vector<float> long_depth = {0.0f, 0.5f, 1.0f, 0.25f, 0.75f};
    result = svp::vision::float_depth_to_uint16(long_depth, 2, 2);
    require(result.empty(),
            "oversized ONNX output must return empty, not truncate");

    // Empty input: must return empty
    std::vector<float> empty_depth;
    result = svp::vision::float_depth_to_uint16(empty_depth, 2, 2);
    require(result.empty(),
            "empty ONNX output must return empty");
  }

  // Cleanup
  std::filesystem::remove_all(tmp_dir);

  std::cout << "depth_generation_boundary_tests: all checks passed\n";
  return 0;
}
