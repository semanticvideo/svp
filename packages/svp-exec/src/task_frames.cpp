#include "svp/exec/task_frames.hpp"

#include "json_fields.hpp"
#include "svp/exec/exec_error.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace svp::exec {
namespace {

constexpr std::string_view kTaskSpecMember = "task_spec";
constexpr std::string_view kTaskResultMember = "task_result";

const nlohmann::json& single_member_body(const Frame& frame,
                                         MessageType expected_type,
                                         std::string_view member) {
  if (frame.type != expected_type) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    "expected a " + std::string(message_type_name(expected_type)) +
                        " frame, got " +
                        std::string(message_type_name(frame.type)));
  }
  const std::string path = std::string(message_type_name(expected_type)) + ".body";
  detail::require_object(frame.body, path);
  detail::reject_unknown_fields(frame.body, {member}, path);
  return detail::required_object(frame.body, member, path);
}

// Payload i must be the bytes of outputs[i]. The caller picks the error codes:
// building a frame from bad local data is invalid_value, receiving one is a
// malformed frame (count) or a hash mismatch (content).
void require_payloads_match(const std::vector<ArtifactRef>& outputs,
                            const std::vector<FramePayload>& payloads,
                            ExecErrorCode count_code,
                            ExecErrorCode content_code) {
  if (payloads.size() != outputs.size()) {
    throw ExecError(count_code, "RESULT frame carries " +
                                    std::to_string(payloads.size()) +
                                    " payloads for " +
                                    std::to_string(outputs.size()) + " outputs");
  }
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    if (payloads[index].size() != outputs[index].bytes ||
        blake3_digest(payloads[index]) != outputs[index].blake3) {
      throw ExecError(content_code, "RESULT payload " + std::to_string(index) +
                                        " does not match outputs[" +
                                        std::to_string(index) + "]");
    }
  }
}

}  // namespace

Frame make_assign_frame(const TaskSpec& spec) {
  return Frame{.type = MessageType::assign,
               .body = nlohmann::json{{kTaskSpecMember, task_spec_to_json(spec)}},
               .payloads = {}};
}

TaskSpec task_spec_from_assign_frame(const Frame& frame) {
  const nlohmann::json& spec =
      single_member_body(frame, MessageType::assign, kTaskSpecMember);
  if (!frame.payloads.empty()) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    "ASSIGN frames carry no payloads");
  }
  return task_spec_from_json(spec);
}

Frame make_result_frame(const TaskResult& result,
                        std::vector<FramePayload> output_payloads) {
  require_payloads_match(result.outputs, output_payloads,
                         ExecErrorCode::invalid_value,
                         ExecErrorCode::invalid_value);
  return Frame{
      .type = MessageType::result,
      .body = nlohmann::json{{kTaskResultMember, task_result_to_json(result)}},
      .payloads = std::move(output_payloads)};
}

TaskResult task_result_from_result_frame(const Frame& frame) {
  TaskResult result = task_result_from_json(
      single_member_body(frame, MessageType::result, kTaskResultMember));
  require_payloads_match(result.outputs, frame.payloads,
                         ExecErrorCode::frame_malformed,
                         ExecErrorCode::payload_hash_mismatch);
  return result;
}

}  // namespace svp::exec
