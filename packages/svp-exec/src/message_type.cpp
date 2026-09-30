#include "svp/exec/message_type.hpp"

namespace svp::exec {

std::string_view message_type_name(MessageType type) noexcept {
  switch (type) {
    case MessageType::hello:
      return "HELLO";
    case MessageType::hello_ack:
      return "HELLO_ACK";
    case MessageType::runtime_have:
      return "RUNTIME_HAVE";
    case MessageType::runtime_put:
      return "RUNTIME_PUT";
    case MessageType::blob_have:
      return "BLOB_HAVE";
    case MessageType::blob_put:
      return "BLOB_PUT";
    case MessageType::calibrate:
      return "CALIBRATE";
    case MessageType::assign:
      return "ASSIGN";
    case MessageType::accept:
      return "ACCEPT";
    case MessageType::reject:
      return "REJECT";
    case MessageType::heartbeat:
      return "HEARTBEAT";
    case MessageType::result:
      return "RESULT";
    case MessageType::cancel:
      return "CANCEL";
    case MessageType::drain:
      return "DRAIN";
    case MessageType::shutdown:
      return "SHUTDOWN";
    case MessageType::unpair:
      return "UNPAIR";
    case MessageType::error:
      return "ERROR";
  }
  return "UNKNOWN";
}

std::optional<MessageType> parse_message_type(std::string_view name) noexcept {
  for (const MessageType type : kAllMessageTypes) {
    if (message_type_name(type) == name) {
      return type;
    }
  }
  return std::nullopt;
}

}  // namespace svp::exec
