#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace svp::exec {

// Frame message types (plan §4.3). The wire name is the upper-case spelling
// returned by message_type_name(). Query/answer pairs the plan writes as
// `RUNTIME_HAVE?` / `BLOB_HAVE?` share one type each; the direction is carried
// by the message body, which later protocol work defines.
enum class MessageType {
  hello,
  hello_ack,
  runtime_have,
  runtime_put,
  blob_have,
  blob_put,
  calibrate,
  assign,
  accept,
  reject,
  heartbeat,
  result,
  cancel,
  drain,
  shutdown,
  unpair,
  error,
};

inline constexpr std::array kAllMessageTypes = {
    MessageType::hello,     MessageType::hello_ack, MessageType::runtime_have,
    MessageType::runtime_put, MessageType::blob_have, MessageType::blob_put,
    MessageType::calibrate, MessageType::assign,    MessageType::accept,
    MessageType::reject,    MessageType::heartbeat, MessageType::result,
    MessageType::cancel,    MessageType::drain,     MessageType::shutdown,
    MessageType::unpair,    MessageType::error,
};

[[nodiscard]] std::string_view message_type_name(MessageType type) noexcept;

// Exact, case-sensitive match of a wire name; nullopt for anything else.
[[nodiscard]] std::optional<MessageType> parse_message_type(
    std::string_view name) noexcept;

}  // namespace svp::exec
