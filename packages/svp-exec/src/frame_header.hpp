#pragma once

// The canonical JSON header that leads every frame, shared by the encoder and
// the incremental decoder so both enforce the same rules.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/frame_limits.hpp"
#include "svp/exec/message_type.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::detail {

struct PayloadDeclaration {
  std::uint64_t bytes = 0;
  Blake3Digest blake3{};
};

struct FrameHeader {
  MessageType type = MessageType::error;
  nlohmann::json body = nlohmann::json::object();
  std::vector<PayloadDeclaration> payloads;
};

// Throws ExecError(frame_payload_too_large) when a declaration or the sum of
// declarations exceeds `limits`.
void check_payload_limits(const std::vector<PayloadDeclaration>& payloads,
                          const FrameLimits& limits);

// Returns the canonical header bytes; throws ExecError(frame_header_too_large)
// or (frame_payload_too_large) when `limits` are exceeded.
[[nodiscard]] std::string encode_frame_header(const FrameHeader& header,
                                              const FrameLimits& limits);

// Strict inverse of encode_frame_header for bytes already known to fit
// limits.max_header_bytes.
[[nodiscard]] FrameHeader decode_frame_header(std::string_view bytes,
                                              const FrameLimits& limits);

[[nodiscard]] std::uint64_t declared_payload_bytes(
    const std::vector<PayloadDeclaration>& payloads) noexcept;

}  // namespace svp::exec::detail
