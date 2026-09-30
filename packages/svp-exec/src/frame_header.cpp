#include "frame_header.hpp"

#include "json_fields.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"

#include <string>

namespace svp::exec::detail {
namespace {

constexpr std::string_view kRoot = "frame_header";

}  // namespace

std::uint64_t declared_payload_bytes(
    const std::vector<PayloadDeclaration>& payloads) noexcept {
  std::uint64_t total = 0;
  for (const PayloadDeclaration& payload : payloads) {
    total += payload.bytes;
  }
  return total;
}

void check_payload_limits(const std::vector<PayloadDeclaration>& payloads,
                          const FrameLimits& limits) {
  std::uint64_t total = 0;
  for (const PayloadDeclaration& payload : payloads) {
    if (payload.bytes > limits.max_payload_bytes) {
      throw ExecError(ExecErrorCode::frame_payload_too_large,
                      "frame payload of " + std::to_string(payload.bytes) +
                          " bytes exceeds the " +
                          std::to_string(limits.max_payload_bytes) +
                          "-byte payload limit");
    }
    // Written as a subtraction so the running total can never overflow.
    if (total > limits.max_total_payload_bytes ||
        payload.bytes > limits.max_total_payload_bytes - total) {
      throw ExecError(ExecErrorCode::frame_payload_too_large,
                      "frame payloads exceed the " +
                          std::to_string(limits.max_total_payload_bytes) +
                          "-byte per-frame limit");
    }
    total += payload.bytes;
  }
}

std::string encode_frame_header(const FrameHeader& header,
                                const FrameLimits& limits) {
  if (!header.body.is_object()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    "frame_header.body must be a JSON object");
  }
  check_payload_limits(header.payloads, limits);
  nlohmann::json declarations = nlohmann::json::array();
  for (const PayloadDeclaration& payload : header.payloads) {
    declarations.push_back(nlohmann::json{
        {"blake3", blake3_hex(payload.blake3)}, {"bytes", payload.bytes}});
  }
  std::string bytes = encode_canonical_json(nlohmann::json{
      {"body", header.body},
      {"payloads", std::move(declarations)},
      {"type", std::string(message_type_name(header.type))}});
  if (bytes.size() > limits.max_header_bytes) {
    throw ExecError(ExecErrorCode::frame_header_too_large,
                    "frame header of " + std::to_string(bytes.size()) +
                        " bytes exceeds the " +
                        std::to_string(limits.max_header_bytes) +
                        "-byte header limit");
  }
  return bytes;
}

FrameHeader decode_frame_header(std::string_view bytes,
                                const FrameLimits& limits) {
  const nlohmann::json value = decode_canonical_json(bytes);
  require_object(value, kRoot);
  reject_unknown_fields(value, {"body", "payloads", "type"}, kRoot);

  FrameHeader header;
  const std::string type_name = required_string(value, "type", kRoot);
  const auto type = parse_message_type(type_name);
  if (!type) {
    throw ExecError(ExecErrorCode::unknown_message_type,
                    "frame_header.type is not a known message type: `" +
                        type_name + "`");
  }
  header.type = *type;
  header.body = required_object(value, "body", kRoot);

  const std::string payloads_path = child_path(kRoot, "payloads[]");
  for (const nlohmann::json& declaration :
       required_array(value, "payloads", kRoot)) {
    require_object(declaration, payloads_path);
    reject_unknown_fields(declaration, {"blake3", "bytes"}, payloads_path);
    header.payloads.push_back(PayloadDeclaration{
        .bytes = required_unsigned(declaration, "bytes", payloads_path),
        .blake3 = required_blake3_hex(declaration, "blake3", payloads_path)});
  }
  check_payload_limits(header.payloads, limits);
  return header;
}

}  // namespace svp::exec::detail
