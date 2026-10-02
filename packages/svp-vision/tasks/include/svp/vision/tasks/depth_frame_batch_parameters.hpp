#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/tasks/onnx_model_parameters.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Task type identity (plan §4.2): a batch of canonical frames to run depth
// on (svp/vision/depth_frame_work.hpp).
inline constexpr std::string_view kDepthFrameBatchTaskType = "depth.frame_batch";
inline constexpr std::uint64_t kDepthFrameBatchTaskTypeVersion = 1;
// TaskSpec input holding the source media bytes (role
// kOcrFrameBatchSourceRole).
inline constexpr std::string_view kDepthFrameBatchSourceInput = "source";
// Outputs: one record per frame (JSONL), and the uint16 depth fields back to
// back, little-endian.
inline constexpr std::string_view kDepthFrameRecordsRole = "depth_frame_records";
inline constexpr std::string_view kDepthFrameFieldsRole = "depth_frame_fields";

// One canonical frame: where it is in the source, the raster it is decoded
// at, and the BLAKE3 of its RGB24 pixels as the coordinator decoded them, so
// an executor runs depth on exactly those pixels.
struct DepthFrameItem {
  // Position of the frame among the decoded canonical frames.
  std::uint64_t ordinal = 0;
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  int width = 0;
  int height = 0;
  svp::exec::Blake3Digest pixels_blake3{};

  bool operator==(const DepthFrameItem&) const = default;
};

struct DepthFrameBatchParameters {
  std::vector<DepthFrameItem> frames;
  OnnxModelParameters model;
  // ffmpeg_build_identity() of the coordinator's ffmpeg ("b3:<hex>").
  std::string ffmpeg_build;
};

// BLAKE3 of a frame's pixels as RGB24 bytes, row-major.
[[nodiscard]] svp::exec::Blake3Digest canonical_frame_pixels_blake3(
    const std::vector<Srgb8Pixel>& pixels);

// The DepthFrameItem of one decoded canonical frame.
[[nodiscard]] DepthFrameItem depth_frame_item(const ColorRasterFrame& frame,
                                              std::uint64_t ordinal);

// Canonical parameters object:
//   {"decode":{"ffmpeg_build"},"execution_provider",
//    "frames":[{"frame_id","height","ordinal","pixels_blake3","timestamp_us",
//               "width"}, ...],
//    "model_id","threads":{"inter_op","intra_op"}}
// Throws std::invalid_argument when the values break the schema below.
[[nodiscard]] nlohmann::json depth_frame_batch_parameters_to_json(
    const DepthFrameBatchParameters& parameters);
[[nodiscard]] DepthFrameBatchParameters depth_frame_batch_parameters_from_json(
    const nlohmann::json& value);

// nullopt when valid: known fields only, frames non-empty with strictly
// ascending ordinals, non-negative timestamps, a size of at least 1x1, a
// 64-hex pixels digest, the model fields, and a decoder "b3:<64 hex>".
[[nodiscard]] std::optional<std::string> validate_depth_frame_batch_parameters(
    const nlohmann::json& value);

[[nodiscard]] std::uint64_t depth_frame_parameter_bytes(const DepthFrameItem& item);

}  // namespace svp::vision::tasks
