#pragma once

// TaskResult construction shared by the audio task types: each returns one
// CBOR output, and fails without outputs.

#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace svp::audio::tasks::detail {

[[nodiscard]] svp::exec::TaskResult failed_result(const svp::exec::TaskSpec& spec,
                                                  std::string code, std::string message,
                                                  bool retryable);

[[nodiscard]] svp::exec::TaskResult cbor_result(const svp::exec::TaskSpec& spec,
                                                const AudioTaskEnvironment& environment,
                                                const nlohmann::json& body,
                                                std::string_view media_type,
                                                std::string_view role,
                                                nlohmann::json diagnostics);

// The body of a committed result's single CBOR output with `role`. Throws
// std::invalid_argument naming the task otherwise.
[[nodiscard]] nlohmann::json read_cbor_output(const svp::exec::TaskSpec& spec,
                                              const std::vector<svp::exec::ArtifactRef>& outputs,
                                              const std::vector<std::vector<std::byte>>& payloads,
                                              std::string_view role);

}  // namespace svp::audio::tasks::detail
