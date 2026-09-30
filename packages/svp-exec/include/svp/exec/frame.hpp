#pragma once

#include "svp/exec/frame_limits.hpp"
#include "svp/exec/message_type.hpp"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <vector>

namespace svp::exec {

using FramePayload = std::vector<std::byte>;

// One protocol message (plan §4.3). Wire layout:
//
//   u32 big-endian H | H bytes canonical JSON header | payload 0 | payload 1 ...
//
// header = {"body":{...},
//           "payloads":[{"blake3":"<64 hex>","bytes":<uint>}, ...],
//           "type":"<MESSAGE_TYPE>"}
//
// Payloads follow the header back to back, in declaration order, with no
// padding; each declaration gives that payload's exact length and BLAKE3.
// The header must be canonical JSON, H must be at least 1, and every limit in
// FrameLimits applies. `body` is an object whose meaning belongs to the
// message type.
struct Frame {
  MessageType type = MessageType::error;
  nlohmann::json body = nlohmann::json::object();
  std::vector<FramePayload> payloads;

  bool operator==(const Frame&) const = default;
};

// Throws ExecError(frame_header_too_large / frame_payload_too_large) when the
// frame breaks `limits`, and the canonical JSON errors for a bad body.
[[nodiscard]] std::vector<std::byte> encode_frame(const Frame& frame,
                                                  const FrameLimits& limits = {});

}  // namespace svp::exec
