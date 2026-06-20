#include "svp/vision/depth_generation.hpp"

#include "svp/models/cache.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/models/verification.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace svp::vision {
namespace {

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

std::string hash_to_hex(const std::array<std::uint8_t, 32>& hash) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < hash.size(); ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(hash[i]);
  }
  return oss.str();
}

// Convert sRGB8 pixel buffer to normalized float CHW format for ONNX input.
// Depth Anything V2 expects NCHW float32 with 3 channels, ImageNet normalization.
std::vector<float> frame_to_normalized_chw(
    const ColorRasterFrame& frame) {
  const std::size_t pixel_count =
      static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
  std::vector<float> output(pixel_count * 3);
  constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
  constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};
  for (std::size_t i = 0; i < pixel_count; ++i) {
    output[i] = (static_cast<float>(frame.pixels[i].r) / 255.0f - kMean[0]) / kStd[0];
    output[pixel_count + i] = (static_cast<float>(frame.pixels[i].g) / 255.0f - kMean[1]) / kStd[1];
    output[2 * pixel_count + i] = (static_cast<float>(frame.pixels[i].b) / 255.0f - kMean[2]) / kStd[2];
  }
  return output;
}

// Bilinear resize a single-channel depth map from (src_w x src_h) to (dst_w x dst_h).
// The ONNX model outputs at 14*floor(h/14) x 14*floor(w/14); we resize back to
// the original frame dimensions so the SVPB depth block matches the canonical raster.
std::vector<float> bilinear_resize_depth(
    const std::vector<float>& src,
    std::uint32_t src_w, std::uint32_t src_h,
    std::uint32_t dst_w, std::uint32_t dst_h) {
  if (src_w == dst_w && src_h == dst_h) {
    return src;
  }
  std::vector<float> dst(static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h));
  const float x_ratio = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float y_ratio = static_cast<float>(src_h) / static_cast<float>(dst_h);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    const float src_y = (y + 0.5f) * y_ratio - 0.5f;
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(src_y)));
    const std::uint32_t y1 = std::min(y0 + 1, src_h - 1);
    const float wy = std::max(0.0f, std::min(1.0f, src_y - static_cast<float>(y0)));
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const float src_x = (x + 0.5f) * x_ratio - 0.5f;
      const std::uint32_t x0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(src_x)));
      const std::uint32_t x1 = std::min(x0 + 1, src_w - 1);
      const float wx = std::max(0.0f, std::min(1.0f, src_x - static_cast<float>(x0)));
      const float v00 = src[static_cast<std::size_t>(y0) * src_w + x0];
      const float v01 = src[static_cast<std::size_t>(y0) * src_w + x1];
      const float v10 = src[static_cast<std::size_t>(y1) * src_w + x0];
      const float v11 = src[static_cast<std::size_t>(y1) * src_w + x1];
      const float v0 = v00 * (1.0f - wx) + v01 * wx;
      const float v1 = v10 * (1.0f - wx) + v11 * wx;
      dst[static_cast<std::size_t>(y) * dst_w + x] = v0 * (1.0f - wy) + v1 * wy;
    }
  }
  return dst;
}

}  // namespace

// Convert float depth output to uint16 payload for SVPB block writing.
// The ONNX model outputs float depth; we quantize to uint16 relative inverse depth.
// Returns empty vector if the output size does not exactly match the expected
// raster dimensions — mismatched output must not be zero-padded into a real block.
std::vector<std::uint16_t> float_depth_to_uint16(
    const std::vector<float>& depth,
    std::uint32_t width,
    std::uint32_t height) {
  const std::size_t expected =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (depth.size() != expected) {
    return {};
  }
  std::vector<std::uint16_t> output(expected, 0);
  for (std::size_t i = 0; i < expected; ++i) {
    float v = depth[i];
    if (std::isnan(v) || std::isinf(v)) {
      v = 0.0f;
    }
    // Clamp to [0, 1] and scale to uint16 range.
    v = std::max(0.0f, std::min(1.0f, v));
    output[i] = static_cast<std::uint16_t>(v * 65535.0f);
  }
  return output;
}

DepthGenerationResult generate_depth_blocks(
    const DepthGenerationOptions& options,
    const std::filesystem::path& staging_dir) {
  DepthGenerationResult result;
  result.onnx_runtime_available = svp::models::OnnxSession::is_available();
  result.model_id = options.model_id;
  result.execution_provider = options.execution_provider;

  // Report frame input availability honestly and independently of model gating.
  // This is set before any model checks so it reflects the true state of
  // frame decoding even when depth generation is blocked by missing models.
  result.depth_frame_input_available =
      options.frame_input.decoding_succeeded &&
      !options.frame_input.frames.empty();

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

  // BLAKE3 verification of model bundle files before any ONNX execution
  auto verify_report = svp::models::verify_manifest_files(manifest, *bundle_dir);
  if (!verify_report.ok()) {
    std::string verify_errors;
    for (const auto& issue : verify_report.issues) {
      if (issue.severity == svp::models::VerificationSeverity::error) {
        verify_errors += issue.message + "; ";
      }
    }
    result.blocker = "Model bundle BLAKE3 verification failed: " + verify_errors;
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  result.depth_model_verified = true;

  // Check if real decoded canonical frame input is available
  // (depth_frame_input_available was already set above, independent of model gating)
  if (!result.depth_frame_input_available) {
    if (!options.frame_input.decoding_attempted) {
      result.blocker = "Real decoded/canonical video frame input was not attempted; "
          "depth generation is blocked: " + options.frame_input.skipped_reason;
    } else if (!options.frame_input.decoding_succeeded) {
      result.blocker = "Real decoded/canonical video frame input failed; "
          "depth generation is blocked: " + options.frame_input.skipped_reason;
    } else {
      result.blocker = "Real decoded/canonical video frame input produced no frames; "
          "depth generation is blocked";
    }
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  // Load ONNX session and run depth inference on each decoded frame
  svp::models::OnnxSession session;
  try {
    svp::models::OnnxSessionOptions session_opts;
    session_opts.execution_provider = options.execution_provider;
    session = svp::models::OnnxSession::load(manifest, *bundle_dir, session_opts);
  } catch (const std::exception& e) {
    result.blocker = std::string("Failed to load ONNX session: ") + e.what();
    result.processor_provenance = make_depth_processor_provenance(
        manifest.model_id, manifest.model_bundle_id,
        options.execution_provider, "not_run", result.blocker);
    return result;
  }

  // Write depth blocks to SVPB stream
  const std::filesystem::path depth_blocks_path =
      staging_dir / "spatial" / "depth.blocks.svpdz";
  const std::filesystem::path depth_index_path =
      staging_dir / "spatial" / "depth.index.jsonl";
  std::filesystem::create_directories(depth_blocks_path.parent_path());

  std::vector<std::byte> block_stream;
  std::vector<nlohmann::json> index_entries;

  for (std::size_t frame_idx = 0; frame_idx < options.frame_input.frames.size(); ++frame_idx) {
    const ColorRasterFrame& frame = options.frame_input.frames[frame_idx];

    // Prepare input tensor: normalized CHW float32
    std::vector<float> input_data = frame_to_normalized_chw(frame);

    // Run ONNX depth inference
    std::vector<float> depth_output;
    try {
      depth_output = session.run_depth(
          input_data.data(), input_data.size(),
          static_cast<std::uint32_t>(frame.width),
          static_cast<std::uint32_t>(frame.height));
    } catch (const std::exception& e) {
      result.blocker = std::string("ONNX depth inference failed on frame ") +
          frame.frame_id + ": " + e.what();
      result.processor_provenance = make_depth_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    // The ONNX model outputs at 14*floor(h/14) x 14*floor(w/14), which may
    // differ from the input frame dimensions.  Resize the depth output back
    // to the canonical raster dimensions using bilinear interpolation so the
    // SVPB depth block matches the frame geometry.
    //
    // Determine the actual output dimensions from the element count and the
    // known model output shape formula.
    const std::uint32_t out_h =
        14u * static_cast<std::uint32_t>(frame.height / 14);
    const std::uint32_t out_w =
        14u * static_cast<std::uint32_t>(frame.width / 14);

    std::vector<float> resized_depth = depth_output;
    if (out_w > 0 && out_h > 0 &&
        depth_output.size() == static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) &&
        (out_w != static_cast<std::uint32_t>(frame.width) ||
         out_h != static_cast<std::uint32_t>(frame.height))) {
      resized_depth = bilinear_resize_depth(
          depth_output, out_w, out_h,
          static_cast<std::uint32_t>(frame.width),
          static_cast<std::uint32_t>(frame.height));
    }

    // Convert float depth to uint16 payload.
    // Reject mismatched sizes instead of zero-padding.
    std::vector<std::uint16_t> depth_uint16 = float_depth_to_uint16(
        resized_depth,
        static_cast<std::uint32_t>(frame.width),
        static_cast<std::uint32_t>(frame.height));

    if (depth_uint16.empty()) {
      result.blocker = std::string("ONNX depth output size mismatch for frame ") +
          frame.frame_id + ": expected " +
          std::to_string(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)) +
          " elements but got " + std::to_string(depth_output.size()) +
          " (resized to " + std::to_string(resized_depth.size()) + ")" +
          "; depth generation blocked to prevent fake/partial depth";
      result.processor_provenance = make_depth_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    // Write SVPB block
    svp::blocks::BlockWriteSpec spec;
    spec.block_type = svp::blocks::BlockType::depth;
    spec.extent_0 = static_cast<std::uint32_t>(frame.width);
    spec.extent_1 = static_cast<std::uint32_t>(frame.height);
    spec.extent_2 = 1;
    spec.dtype = svp::blocks::DType::uint16;
    spec.start_frame = frame_idx;
    spec.frame_count = 1;
    // For single-frame blocks, use absent time range (-1) since the validator
    // requires end_us > start_us and a single frame has no duration span.
    spec.start_us = -1;
    spec.end_us = -1;

    svp::blocks::WrittenBlockInfo block_info;
    try {
      block_info = svp::blocks::write_block(
          block_stream, spec,
          reinterpret_cast<const std::byte*>(depth_uint16.data()),
          depth_uint16.size() * sizeof(std::uint16_t));
    } catch (const std::exception& e) {
      result.blocker = std::string("Failed to write depth block for frame ") +
          frame.frame_id + ": " + e.what();
      result.processor_provenance = make_depth_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }

    const std::string payload_hex = hash_to_hex(block_info.payload_blake3);
    const std::string block_hex = hash_to_hex(block_info.header_blake3);

    DepthBlockEntry entry;
    entry.id = "depth_" + frame.frame_id;
    entry.frame_id = frame.frame_id;
    entry.width = static_cast<std::uint32_t>(frame.width);
    entry.height = static_cast<std::uint32_t>(frame.height);
    entry.block_offset = block_info.block_offset;
    entry.block_length = block_info.block_length;
    entry.payload_offset = block_info.payload_offset;
    entry.uncompressed_size = block_info.uncompressed_size;
    entry.compressed_size = block_info.compressed_size;
    entry.processor_id = "proc_depth_0001";
    entry.payload_blake3 = payload_hex;
    entry.block_blake3 = block_hex;
    entry.start_frame = frame_idx;
    entry.frame_count = 1;
    entry.start_us = frame.timestamp_us;
    entry.end_us = frame.timestamp_us;
    result.entries.push_back(entry);

    index_entries.push_back({
        {"id", entry.id},
        {"frame_id", entry.frame_id},
        {"width", entry.width},
        {"height", entry.height},
        {"value_type", entry.value_type},
        {"normalization", entry.normalization},
        {"block_file", entry.block_file},
        {"block_offset", entry.block_offset},
        {"block_length", entry.block_length},
        {"payload_offset", entry.payload_offset},
        {"uncompressed_size", entry.uncompressed_size},
        {"compressed_size", entry.compressed_size},
        {"payload_blake3", entry.payload_blake3},
        {"block_blake3", entry.block_blake3},
        {"start_frame", entry.start_frame},
        {"frame_count", entry.frame_count},
        {"start_us", entry.start_us},
        {"end_us", entry.end_us}
    });
  }

  // Write block stream to file
  {
    std::ofstream out(depth_blocks_path, std::ios::binary);
    if (!out) {
      result.blocker = "Failed to open depth blocks file: " + depth_blocks_path.string();
      result.processor_provenance = make_depth_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }
    out.write(reinterpret_cast<const char*>(block_stream.data()),
              static_cast<std::streamsize>(block_stream.size()));
    if (!out) {
      result.blocker = "Failed to write depth blocks file: " + depth_blocks_path.string();
      result.processor_provenance = make_depth_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }
  }
  result.depth_blocks_written = true;

  // Write index JSONL
  {
    std::filesystem::create_directories(depth_index_path.parent_path());
    std::ofstream out(depth_index_path);
    if (!out) {
      result.blocker = "Failed to open depth index file: " + depth_index_path.string();
      result.processor_provenance = make_depth_processor_provenance(
          manifest.model_id, manifest.model_bundle_id,
          options.execution_provider, "error", result.blocker);
      return result;
    }
    for (const nlohmann::json& entry : index_entries) {
      out << entry.dump() << "\n";
    }
  }
  result.depth_index_written = true;
  result.depth_generation_run = true;

  result.processor_provenance = make_depth_processor_provenance(
      manifest.model_id, manifest.model_bundle_id,
      options.execution_provider, "completed",
      "Depth blocks generated from " + std::to_string(result.entries.size()) +
      " real decoded canonical frame(s) using ONNX Runtime");

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
      {"depth_model_verified", result.depth_model_verified},
      {"depth_frame_input_available", result.depth_frame_input_available},
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
