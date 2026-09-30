#include "svp/exec/frame.hpp"

#include "frame_header.hpp"

#include <string>

namespace svp::exec {
namespace {

void append_big_endian_u32(std::vector<std::byte>& output, std::uint32_t value) {
  for (std::size_t index = kFrameLengthPrefixBytes; index > 0; --index) {
    output.push_back(static_cast<std::byte>((value >> ((index - 1) * 8U)) & 0xFFU));
  }
}

}  // namespace

std::vector<std::byte> encode_frame(const Frame& frame, const FrameLimits& limits) {
  detail::FrameHeader header{.type = frame.type, .body = frame.body, .payloads = {}};
  for (const FramePayload& payload : frame.payloads) {
    header.payloads.push_back(detail::PayloadDeclaration{
        .bytes = payload.size(), .blake3 = blake3_digest(payload)});
  }
  const std::string header_bytes = detail::encode_frame_header(header, limits);

  std::vector<std::byte> output;
  output.reserve(kFrameLengthPrefixBytes + header_bytes.size() +
                 static_cast<std::size_t>(
                     detail::declared_payload_bytes(header.payloads)));
  append_big_endian_u32(output, static_cast<std::uint32_t>(header_bytes.size()));
  for (const char character : header_bytes) {
    output.push_back(static_cast<std::byte>(character));
  }
  for (const FramePayload& payload : frame.payloads) {
    output.insert(output.end(), payload.begin(), payload.end());
  }
  return output;
}

}  // namespace svp::exec
