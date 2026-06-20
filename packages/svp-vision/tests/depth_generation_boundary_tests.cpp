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
            "JSON: depth_frame_input_available must be false");
    require(json.at("depth_generation_run") == false,
            "JSON: depth_generation_run must be false");
    require(!json.at("blocker").get<std::string>().empty(),
            "JSON: blocker must be non-empty when model is missing");
  }

  // Test 2: Model present but no frame input — depth should remain blocked
  // on missing frame input, not on missing model.
  // We cannot easily create a real model bundle in a unit test, but we can
  // verify that the blocker message changes from "model not found" to
  // a frame-input-related message when the model exists but frames don't.
  // Since we don't have a real model bundle, this test verifies the
  // no-frame-input path with a missing model (the first check that fires).
  {
    svp::vision::DepthGenerationOptions opts;
    opts.model_cache_root = tmp_dir / "nonexistent_cache";
    opts.model_id = "model_depth_anything_v2_small";
    // Provide empty frame_input (default-constructed)
    // decoding_succeeded is false, frames is empty

    svp::vision::DepthGenerationResult result =
        svp::vision::generate_depth_blocks(opts, tmp_dir / "test_no_frames");

    require(!result.depth_generation_run,
            "depth generation must not run without frames");
    require(!result.depth_frame_input_available,
            "depth_frame_input_available must be false without frames");
    require(!result.blocker.empty(),
            "blocker must be set without frames");
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

    // Model is missing, so it blocks on model first
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

  // Cleanup
  std::filesystem::remove_all(tmp_dir);

  std::cout << "depth_generation_boundary_tests: all checks passed\n";
  return 0;
}
