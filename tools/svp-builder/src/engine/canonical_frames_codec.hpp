#pragma once

// The canonical frames decoded once per build and shared by depth and OCR
// cross the task boundary as two state blobs: a JSON index (decode status and
// per-frame metadata) and the frames' RGB pixels back to back.

#include "svp/vision/canonical_frame_input.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>

namespace svp::builder::engine {

struct EncodedCanonicalFrames {
  nlohmann::json index;
  std::vector<std::byte> pixels;
};

[[nodiscard]] EncodedCanonicalFrames encode_canonical_frames(
    const svp::vision::DecodedCanonicalFrames& frames);

// Throws std::runtime_error when the pixel bytes do not match the index.
[[nodiscard]] svp::vision::DecodedCanonicalFrames decode_canonical_frames_state(
    const nlohmann::json& index, const std::vector<std::byte>& pixels);

}  // namespace svp::builder::engine
