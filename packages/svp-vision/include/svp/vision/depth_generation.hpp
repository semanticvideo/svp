#pragma once

#include "svp/models/reference_processor_model_ids.hpp"

#include "svp/blocks/block_writer.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/canonical_frame_input.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

struct DepthInferenceRuntime {
  std::unique_ptr<svp::models::OnnxSession> session;
  std::string model_id;
  std::string model_bundle_id;
  std::string execution_provider;
  std::string blocker;
};

[[nodiscard]] DepthInferenceRuntime load_depth_inference_runtime(
    const std::filesystem::path& model_cache_root,
    const std::string& model_id,
    const std::string& execution_provider);

[[nodiscard]] std::vector<std::uint16_t> infer_depth_frame(
    DepthInferenceRuntime& runtime,
    const ColorRasterFrame& frame);

struct DepthGenerationOptions {
  std::filesystem::path model_cache_root;
  std::string model_id = svp::models::kDepthAnythingV2SmallModelId;
  std::string execution_provider = "cpu";
  std::uint32_t raster_width = 0;
  std::uint32_t raster_height = 0;
  DecodedCanonicalFrames frame_input;
  std::function<void(std::size_t current, std::size_t total)> on_progress;
};

struct DepthBlockEntry {
  std::string id;
  std::string frame_id;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string value_type = "uint16_relative_inverse_depth";
  std::string normalization = "near_is_larger";
  std::string block_file = "spatial/depth.blocks.svpdz";
  std::uint64_t block_offset = 0;
  std::uint64_t block_length = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t compressed_size = 0;
  std::string processor_id;
  std::string payload_blake3;
  std::string block_blake3;
  std::uint64_t start_frame = 0;
  std::uint64_t frame_count = 0;
  std::int64_t start_us = -1;
  std::int64_t end_us = -1;
};

struct DepthGenerationResult {
  bool onnx_runtime_available = false;
  bool depth_model_available = false;
  bool depth_model_verified = false;
  bool depth_frame_input_available = false;
  bool depth_generation_run = false;
  bool depth_blocks_written = false;
  bool depth_index_written = false;
  std::string model_id;
  std::string model_bundle_id;
  std::string execution_provider;
  std::string blocker;
  std::vector<DepthBlockEntry> entries;
  // Raw per-frame uint16 depth data, concatenated in frame order.
  // Each frame contributes width*height uint16 values.
  // Populated when depth generation succeeds; empty when blocked.
  std::vector<std::uint16_t> raw_depth_data;
  nlohmann::json processor_provenance;
};

[[nodiscard]] DepthGenerationResult generate_depth_blocks(
    const DepthGenerationOptions& options,
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json depth_generation_result_to_json(
    const DepthGenerationResult& result);

// Convert float depth output to uint16 relative inverse depth payload.
// Per-frame normalization: 0 = farthest, 65535 = nearest.
// Returns empty vector (blocking) if:
//   - size does not match width*height
//   - any NaN or Inf present
//   - all values are identical (no meaningful depth variation)
[[nodiscard]] std::vector<std::uint16_t> float_depth_to_uint16(
    const std::vector<float>& depth,
    std::uint32_t width,
    std::uint32_t height);

}  // namespace svp::vision
