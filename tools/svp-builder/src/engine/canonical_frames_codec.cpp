#include "engine/canonical_frames_codec.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace svp::builder::engine {
namespace {

constexpr std::size_t kPixelBytes = sizeof(svp::vision::Srgb8Pixel);
static_assert(kPixelBytes == 3, "Srgb8Pixel must be three packed bytes");

}  // namespace

EncodedCanonicalFrames encode_canonical_frames(
    const svp::vision::DecodedCanonicalFrames& frames) {
  EncodedCanonicalFrames encoded;
  nlohmann::json frame_index = nlohmann::json::array();
  for (const svp::vision::ColorRasterFrame& frame : frames.frames) {
    frame_index.push_back({{"frame_id", frame.frame_id},
                           {"timestamp_us", frame.timestamp_us},
                           {"width", frame.width},
                           {"height", frame.height},
                           {"keyframe", frame.keyframe},
                           {"frame_index", frame.frame_index},
                           {"pixel_count", frame.pixels.size()}});
    const auto* begin = reinterpret_cast<const std::byte*>(frame.pixels.data());
    encoded.pixels.insert(encoded.pixels.end(), begin,
                          begin + frame.pixels.size() * kPixelBytes);
  }
  encoded.index = {{"decoding_attempted", frames.decoding_attempted},
                   {"decoding_succeeded", frames.decoding_succeeded},
                   {"frames_attempted", frames.frames_attempted},
                   {"frames_decoded", frames.frames_decoded},
                   {"frames_missed", frames.frames_missed},
                   {"skipped_reason", frames.skipped_reason},
                   {"frames", frame_index}};
  return encoded;
}

svp::vision::DecodedCanonicalFrames decode_canonical_frames_state(
    const nlohmann::json& index, const std::vector<std::byte>& pixels) {
  svp::vision::DecodedCanonicalFrames frames;
  frames.decoding_attempted = index.at("decoding_attempted").get<bool>();
  frames.decoding_succeeded = index.at("decoding_succeeded").get<bool>();
  frames.frames_attempted = index.at("frames_attempted").get<int>();
  frames.frames_decoded = index.at("frames_decoded").get<int>();
  frames.frames_missed = index.at("frames_missed").get<int>();
  frames.skipped_reason = index.at("skipped_reason").get<std::string>();
  std::size_t offset = 0;
  for (const nlohmann::json& entry : index.at("frames")) {
    svp::vision::ColorRasterFrame frame;
    frame.frame_id = entry.at("frame_id").get<std::string>();
    frame.timestamp_us = entry.at("timestamp_us").get<std::int64_t>();
    frame.width = entry.at("width").get<int>();
    frame.height = entry.at("height").get<int>();
    frame.keyframe = entry.at("keyframe").get<bool>();
    frame.frame_index = entry.at("frame_index").get<std::size_t>();
    const auto pixel_count = entry.at("pixel_count").get<std::size_t>();
    const std::size_t byte_count = pixel_count * kPixelBytes;
    if (offset + byte_count > pixels.size()) {
      throw std::runtime_error("canonical frame pixels are shorter than their index");
    }
    frame.pixels.resize(pixel_count);
    std::memcpy(frame.pixels.data(), pixels.data() + offset, byte_count);
    offset += byte_count;
    frames.frames.push_back(std::move(frame));
  }
  if (offset != pixels.size()) {
    throw std::runtime_error("canonical frame pixels are longer than their index");
  }
  return frames;
}

}  // namespace svp::builder::engine
