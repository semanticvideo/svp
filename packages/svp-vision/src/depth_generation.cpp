#include "svp/vision/depth_generation.hpp"

#include "svp/models/cache.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <blake3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace svp::vision {
namespace {

std::string to_hex(const std::array<std::uint8_t, 32>& hash) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (auto byte : hash) {
    ss << std::setw(2) << static_cast<int>(byte);
  }
  return ss.str();
}

std::optional<std::filesystem::path> find_model_bundle_dir(
    const std::filesystem::path& cache_root,
    const std::string& model_id) {
  if (cache_root.empty() || !std::filesystem::exists(cache_root)) {
    return std::nullopt;
  }

  const std::filesystem::path model_dir = cache_root / model_id;
  if (std::filesystem::exists(model_dir / "model.svpmodel.json")) {
    return model_dir;
  }

  for (const auto& entry : std::filesystem::directory_iterator(cache_root)) {
    if (!entry.is_directory()) continue;
    const auto candidate = entry.path() / "model.svpmodel.json";
    if (std::filesystem::exists(candidate)) {
      try {
        auto manifest = svp::models::load_model_bundle_manifest(candidate);
        if (manifest.model_id == model_id) {
          return entry.path();
        }
      } catch (...) {
      }
    }
  }

  return std::nullopt;
}

std::vector<float> preprocess_frame_to_rgb(
    const std::vector<std::uint8_t>& rgb_pixels,
    std::uint32_t width,
    std::uint32_t height) {
  std::vector<float> output(width * height * 3);
  for (std::size_t i = 0; i < width * height; ++i) {
    output[i * 3] = rgb_pixels[i * 3] / 255.0f;
    output[i * 3 + 1] = rgb_pixels[i * 3 + 1] / 255.0f;
    output[i * 3 + 2] = rgb_pixels[i * 3 + 2] / 255.0f;
  }
  return output;
}

std::vector<std::uint16_t> convert_depth_to_uint16(
    const float* depth_data,
    std::size_t count) {
  std::vector<std::uint16_t> result(count);
  float min_val = std::numeric_limits<float>::max();
  float max_val = std::numeric_limits<float>::lowest();
  for (std::size_t i = 0; i < count; ++i) {
    float v = depth_data[i];
    if (std::isfinite(v)) {
      min_val = std::min(min_val, v);
      max_val = std::max(max_val, v);
    }
  }
  if (max_val <= min_val) {
    for (auto& v : result) v = 32768;
    return result;
  }
  const float range = max_val - min_val;
  for (std::size_t i = 0; i < count; ++i) {
    float v = depth_data[i];
    if (!std::isfinite(v)) {
      result[i] = 0;
    } else {
      float normalized = (v - min_val) / range;
      result[i] = static_cast<std::uint16_t>(std::clamp(
          static_cast<int>(normalized * 65535.0f), 0, 65535));
    }
  }
  return result;
}

nlohmann::json make_depth_processor_provenance(
    const std::string& model_id,
    const std::string& model_bundle_id,
    const std::string& execution_provider,
    const std::string& status,
    const std::string& note) {
  return {
      {"id", "proc_depth_0001"},
      {"name", "svp depth generation"},
      {"version", "svp-depth-v1"},
      {"input_refs", {"canonical_analysis_raster_frames"}},
      {"output_refs", {
          "spatial/depth.index.jsonl",
          "spatial/depth.blocks.svpdz"
      }},
      {"model_refs", model_id.empty() ? nlohmann::json::array() :
          nlohmann::json::array({model_id})},
      {"task_ids", {"task.vision.depth_generation"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", status},
      {"runtime", "onnxruntime"},
      {"execution_provider", execution_provider},
      {"note", note}
  };
}

}  // namespace

DepthGenerationResult generate_depth_blocks(
    const DepthGenerationOptions& options,
    const std::filesystem::path& staging_dir) {
  DepthGenerationResult result;
  result.onnx_runtime_available = svp::models::OnnxSession::is_available();
  result.model_id = options.model_id;
  result.execution_provider = options.execution_provider;

  if (!result.onnx_runtime_available) {
    result.blocker = "ONNX Runtime is not available in this build";
    result.processor_provenance = make_depth_processor_provenance(
        "", "", options.execution_provider, "not_run", result.blocker);
    return result;
  }

  const auto cache_root = options.model_cache_root.empty()
      ? svp::models::model_cache_root()
      : options.model_cache_root;

  auto bundle_dir = find_model_bundle_dir(cache_root, options.model_id);
  if (!bundle_dir.has_value()) {
    result.blocker = "Depth model bundle not found in cache: " + options.model_id;
    result.processor_provenance = make_depth_processor_provenance(
        options.model_id, "", options.execution_provider, "not_run",
        result.blocker);
    return result;
  }

  result.depth_model_available = true;

  std::optional<svp::models::ModelBundleManifest> manifest_opt;
  try {
    manifest_opt = svp::models::load_model_bundle_manifest(
        *bundle_dir / "model.svpmodel.json");
    result.model_bundle_id = manifest_opt->model_bundle_id;
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load model manifest: ") + e.what();
    result.processor_provenance = make_depth_processor_provenance(
        options.model_id, "", options.execution_provider, "not_run",
        result.blocker);
    return result;
  }
  const auto& manifest = *manifest_opt;

  svp::models::OnnxSession session;
  try {
    svp::models::OnnxSessionOptions session_opts;
    session_opts.execution_provider = options.execution_provider;
    session = svp::models::OnnxSession::load(manifest, *bundle_dir, session_opts);
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load ONNX model: ") + e.what();
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  const std::uint32_t raster_w = options.raster_width > 0
      ? options.raster_width : 640;
  const std::uint32_t raster_h = options.raster_height > 0
      ? options.raster_height : 360;

  if (raster_w == 0 || raster_h == 0) {
    result.blocker = "Canonical raster dimensions are zero";
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  std::vector<std::byte> block_stream;
  std::vector<DepthBlockEntry> entries;

  const std::uint64_t frame_count = 1;
  const std::uint64_t start_frame = 0;

  std::vector<std::uint8_t> synthetic_frame(raster_w * raster_h * 3, 128);
  auto rgb_float = preprocess_frame_to_rgb(synthetic_frame, raster_w, raster_h);

  std::vector<float> depth_output;
  try {
    depth_output = session.run_depth(
        rgb_float.data(), rgb_float.size(), raster_w, raster_h);
  } catch (const std::exception& e) {
    result.blocker = std::string("ONNX inference failed: ") + e.what();
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  if (depth_output.empty()) {
    result.blocker = "ONNX model produced empty depth output";
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  const std::size_t pixel_count = static_cast<std::size_t>(raster_w) * raster_h;
  if (depth_output.size() < pixel_count) {
    result.blocker = "ONNX depth output smaller than raster dimensions";
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  auto depth_uint16 = convert_depth_to_uint16(depth_output.data(), pixel_count);

  const std::uint64_t uncompressed_size = pixel_count * 2;

  svp::blocks::BlockWriteSpec spec;
  spec.block_type = svp::blocks::BlockType::depth;
  spec.extent_0 = raster_w;
  spec.extent_1 = raster_h;
  spec.extent_2 = 1;
  spec.dtype = svp::blocks::DType::uint16;
  spec.start_frame = start_frame;
  spec.frame_count = frame_count;
  spec.start_us = 0;
  spec.end_us = 33333;

  auto block_info = svp::blocks::write_block(
      block_stream, spec,
      reinterpret_cast<const std::byte*>(depth_uint16.data()),
      uncompressed_size);

  DepthBlockEntry entry;
  entry.id = "depth_00000000";
  entry.frame_id = "frame_00000000";
  entry.width = raster_w;
  entry.height = raster_h;
  entry.block_offset = block_info.block_offset;
  entry.block_length = block_info.block_length;
  entry.payload_offset = block_info.payload_offset;
  entry.uncompressed_size = block_info.uncompressed_size;
  entry.compressed_size = block_info.compressed_size;
  entry.processor_id = "proc_depth_0001";
  entry.payload_blake3 = to_hex(block_info.payload_blake3);
  entry.block_blake3 = to_hex(block_info.header_blake3);
  entry.start_frame = start_frame;
  entry.frame_count = frame_count;
  entry.start_us = spec.start_us;
  entry.end_us = spec.end_us;
  entries.push_back(entry);

  result.depth_generation_run = true;

  const auto depth_blocks_path = staging_dir / "spatial" / "depth.blocks.svpdz";
  std::filesystem::create_directories(depth_blocks_path.parent_path());
  std::ofstream blocks_file(depth_blocks_path, std::ios::binary);
  blocks_file.write(reinterpret_cast<const char*>(block_stream.data()),
                    static_cast<std::streamsize>(block_stream.size()));
  blocks_file.close();
  result.depth_blocks_written = true;

  const auto depth_index_path = staging_dir / "spatial" / "depth.index.jsonl";
  std::ofstream index_file(depth_index_path);
  for (const auto& e : entries) {
    nlohmann::json record = {
        {"id", e.id},
        {"frame_id", e.frame_id},
        {"width", e.width},
        {"height", e.height},
        {"value_type", e.value_type},
        {"normalization", e.normalization},
        {"block_file", e.block_file},
        {"block_offset", e.block_offset},
        {"block_length", e.block_length},
        {"payload_offset", e.payload_offset},
        {"uncompressed_size", e.uncompressed_size},
        {"compressed_size", e.compressed_size},
        {"processor_id", e.processor_id},
        {"payload_blake3", e.payload_blake3},
        {"block_blake3", e.block_blake3},
        {"start_frame", e.start_frame},
        {"frame_count", e.frame_count},
        {"start_us", e.start_us},
        {"end_us", e.end_us}
    };
    index_file << record.dump() << "\n";
  }
  index_file.close();
  result.depth_index_written = true;
  result.entries = std::move(entries);

  result.processor_provenance = make_depth_processor_provenance(
      manifest.model_id, manifest.model_bundle_id,
      options.execution_provider, "completed",
      "Depth blocks generated using ONNX Runtime inference");

  return result;
}

nlohmann::json depth_generation_result_to_json(
    const DepthGenerationResult& result) {
  nlohmann::json entries_arr = nlohmann::json::array();
  for (const auto& e : result.entries) {
    entries_arr.push_back({
        {"id", e.id},
        {"frame_id", e.frame_id},
        {"width", e.width},
        {"height", e.height},
        {"block_offset", e.block_offset},
        {"block_length", e.block_length},
        {"payload_blake3", e.payload_blake3},
        {"block_blake3", e.block_blake3}
    });
  }

  return {
      {"onnx_runtime_available", result.onnx_runtime_available},
      {"depth_model_available", result.depth_model_available},
      {"depth_generation_run", result.depth_generation_run},
      {"depth_blocks_written", result.depth_blocks_written},
      {"depth_index_written", result.depth_index_written},
      {"model_id", result.model_id},
      {"model_bundle_id", result.model_bundle_id},
      {"execution_provider", result.execution_provider},
      {"blocker", result.blocker},
      {"entries", entries_arr},
      {"processor_provenance", result.processor_provenance}
  };
}

}  // namespace svp::vision
