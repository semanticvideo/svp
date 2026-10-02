#include "svp/vision/tasks/depth_frame_batch_parameters.hpp"

#include "onnx_model_parameters_json.hpp"
#include "parameter_fields.hpp"

#include <stdexcept>

namespace svp::vision::tasks {
namespace {

using detail::Json;

const detail::ParameterFields& fields() {
  static const detail::ParameterFields kFields(kDepthFrameBatchTaskType);
  return kFields;
}

Json frame_to_json(const DepthFrameItem& frame) {
  return Json{{"frame_id", frame.frame_id},
              {"height", frame.height},
              {"ordinal", frame.ordinal},
              {"pixels_blake3", svp::exec::blake3_hex(frame.pixels_blake3)},
              {"timestamp_us", frame.timestamp_us},
              {"width", frame.width}};
}

}  // namespace

svp::exec::Blake3Digest canonical_frame_pixels_blake3(const std::vector<Srgb8Pixel>& pixels) {
  std::vector<std::byte> bytes;
  bytes.reserve(pixels.size() * 3);
  for (const Srgb8Pixel& pixel : pixels) {
    bytes.push_back(static_cast<std::byte>(pixel.r));
    bytes.push_back(static_cast<std::byte>(pixel.g));
    bytes.push_back(static_cast<std::byte>(pixel.b));
  }
  return svp::exec::blake3_digest(std::span<const std::byte>(bytes));
}

DepthFrameItem depth_frame_item(const ColorRasterFrame& frame, std::uint64_t ordinal) {
  return DepthFrameItem{.ordinal = ordinal,
                        .frame_id = frame.frame_id,
                        .timestamp_us = frame.timestamp_us,
                        .width = frame.width,
                        .height = frame.height,
                        .pixels_blake3 = canonical_frame_pixels_blake3(frame.pixels)};
}

std::uint64_t depth_frame_parameter_bytes(const DepthFrameItem& item) {
  return frame_to_json(item).dump().size();
}

Json depth_frame_batch_parameters_to_json(const DepthFrameBatchParameters& parameters) {
  Json frames = Json::array();
  for (const DepthFrameItem& frame : parameters.frames) {
    frames.push_back(frame_to_json(frame));
  }
  Json value{{"decode", {{"ffmpeg_build", parameters.ffmpeg_build}}},
             {"frames", std::move(frames)}};
  detail::add_onnx_model_fields(value, parameters.model);
  (void)depth_frame_batch_parameters_from_json(value);
  return value;
}

DepthFrameBatchParameters depth_frame_batch_parameters_from_json(const Json& value) {
  fields().require_fields(value,
                          {"decode", "execution_provider", "frames", "model_id", "threads"},
                          "parameters");
  DepthFrameBatchParameters parameters;
  parameters.model = detail::onnx_model_fields_from_json(fields(), value);
  const Json& decode = value.at("decode");
  fields().require_fields(decode, {"ffmpeg_build"}, "decode");
  parameters.ffmpeg_build = fields().ffmpeg_build_at(decode, "decode");
  const Json& frames = fields().array_at(value, "frames", "parameters");
  if (frames.empty()) fields().reject("frames must not be empty");
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const std::string where = "frames[" + std::to_string(index) + "]";
    const Json& frame = frames[index];
    fields().require_fields(
        frame, {"frame_id", "height", "ordinal", "pixels_blake3", "timestamp_us", "width"},
        where);
    const auto digest =
        svp::exec::parse_blake3_hex(fields().string_at(frame, "pixels_blake3", where));
    if (!digest) fields().reject(where + ".pixels_blake3 must be 64 hex digits");
    DepthFrameItem item{
        .ordinal = fields().integer_at<std::uint64_t>(frame, "ordinal", where, 0),
        .frame_id = fields().string_at(frame, "frame_id", where),
        .timestamp_us = fields().integer_at<std::int64_t>(frame, "timestamp_us", where, 0),
        .width = fields().integer_at<int>(frame, "width", where, 1),
        .height = fields().integer_at<int>(frame, "height", where, 1),
        .pixels_blake3 = *digest};
    if (!parameters.frames.empty() && item.ordinal <= parameters.frames.back().ordinal) {
      fields().reject(where + " is not strictly after the frame before it");
    }
    parameters.frames.push_back(std::move(item));
  }
  return parameters;
}

std::optional<std::string> validate_depth_frame_batch_parameters(const Json& value) {
  try {
    (void)depth_frame_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string(kDepthFrameBatchTaskType) + " parameters: " + error.what();
  }
}

}  // namespace svp::vision::tasks
