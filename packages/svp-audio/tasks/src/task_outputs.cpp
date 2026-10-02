#include "task_outputs.hpp"

#include "svp/exec/output_digest.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace svp::audio::tasks::detail {
namespace {

// TaskTypeRegistry::execute validates the result before run_task_attempt
// stamps the real attempt number and worker session over these.
constexpr std::uint64_t kUnstampedAttempt = 1;
constexpr std::string_view kUnstampedWorkerSession = "ws_unstamped";

svp::exec::TaskResult unstamped_result(const svp::exec::TaskSpec& spec) {
  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = kUnstampedAttempt;
  result.execution.worker_session_id = std::string(kUnstampedWorkerSession);
  return result;
}

}  // namespace

svp::exec::TaskResult failed_result(const svp::exec::TaskSpec& spec, std::string code,
                                    std::string message, bool retryable) {
  svp::exec::TaskResult result = unstamped_result(spec);
  result.status = svp::exec::TaskStatus::failed;
  result.output_digest = svp::exec::compute_output_digest({});
  result.error = svp::exec::TaskError{.code = std::move(code),
                                      .message = spec.task_type + ": " + std::move(message),
                                      .retryable = retryable};
  return result;
}

svp::exec::TaskResult cbor_result(const svp::exec::TaskSpec& spec,
                                  const AudioTaskEnvironment& environment,
                                  const nlohmann::json& body, std::string_view media_type,
                                  std::string_view role, nlohmann::json diagnostics) {
  const std::vector<std::uint8_t> encoded = nlohmann::json::to_cbor(body);
  std::vector<std::byte> bytes(encoded.size());
  std::memcpy(bytes.data(), encoded.data(), encoded.size());
  svp::exec::TaskResult result = unstamped_result(spec);
  result.status = svp::exec::TaskStatus::succeeded;
  result.outputs = {environment.write_output(bytes, std::string(media_type), std::string(role))};
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  result.diagnostics = std::move(diagnostics);
  return result;
}

nlohmann::json read_cbor_output(const svp::exec::TaskSpec& spec,
                                const std::vector<svp::exec::ArtifactRef>& outputs,
                                const std::vector<std::vector<std::byte>>& payloads,
                                std::string_view role) {
  if (outputs.size() != 1 || payloads.size() != 1 || outputs.front().role != role) {
    throw std::invalid_argument(spec.task_id + ": expected one output `" + std::string(role) +
                                "`");
  }
  const std::vector<std::byte>& payload = payloads.front();
  std::vector<std::uint8_t> bytes(payload.size());
  std::memcpy(bytes.data(), payload.data(), payload.size());
  try {
    return nlohmann::json::from_cbor(bytes);
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument(spec.task_id + ": output is not CBOR: " + error.what());
  }
}

}  // namespace svp::audio::tasks::detail
