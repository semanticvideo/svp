#include "svp/models/thread_plan.hpp"

#include <cstdlib>
#include <string>

namespace svp::models {
namespace {

// Same parse rule the OCR stage applied before plans existed: a base-10
// prefix that is > 0; anything else leaves the planned value in place.
std::optional<int> positive_int(const EnvironmentLookup& lookup,
                                std::string_view name) {
  const std::optional<std::string> value = lookup(name);
  if (!value || value->empty()) return std::nullopt;
  char* end = nullptr;
  const long parsed = std::strtol(value->c_str(), &end, 10);
  if (end == value->c_str() || parsed <= 0) return std::nullopt;
  return static_cast<int>(parsed);
}

// A role-specific variable wins over the shared one, as before.
void apply_ocr_count(int& target, std::string_view field,
                     const EnvironmentLookup& lookup,
                     std::string_view role_variable,
                     std::string_view shared_variable,
                     std::vector<ThreadPlanOverride>& applied) {
  std::string_view variable = role_variable;
  std::optional<int> value = positive_int(lookup, role_variable);
  if (!value) {
    variable = shared_variable;
    value = positive_int(lookup, shared_variable);
  }
  if (!value) return;
  target = *value;
  applied.push_back({.environment_variable = std::string(variable),
                     .field = std::string(field),
                     .value = *value});
}

}  // namespace

EnvironmentLookup process_environment_lookup() {
  return [](std::string_view name) -> std::optional<std::string> {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
  };
}

std::vector<ThreadPlanOverride> apply_thread_plan_environment_overrides(
    ThreadPlan& plan,
    const EnvironmentLookup& lookup,
    bool diagnostic_overrides_enabled) {
  std::vector<ThreadPlanOverride> applied;
  apply_ocr_count(plan.ocr_detection.intra_op,
                  "onnx_runtime.ocr_detection.intra_op", lookup,
                  "SVP_OCR_DET_ONNX_INTRA_OP_THREADS",
                  "SVP_OCR_ONNX_INTRA_OP_THREADS", applied);
  apply_ocr_count(plan.ocr_detection.inter_op,
                  "onnx_runtime.ocr_detection.inter_op", lookup,
                  "SVP_OCR_DET_ONNX_INTER_OP_THREADS",
                  "SVP_OCR_ONNX_INTER_OP_THREADS", applied);
  apply_ocr_count(plan.ocr_recognition.intra_op,
                  "onnx_runtime.ocr_recognition.intra_op", lookup,
                  "SVP_OCR_REC_ONNX_INTRA_OP_THREADS",
                  "SVP_OCR_ONNX_INTRA_OP_THREADS", applied);
  apply_ocr_count(plan.ocr_recognition.inter_op,
                  "onnx_runtime.ocr_recognition.inter_op", lookup,
                  "SVP_OCR_REC_ONNX_INTER_OP_THREADS",
                  "SVP_OCR_ONNX_INTER_OP_THREADS", applied);
  if (diagnostic_overrides_enabled) {
    constexpr std::string_view kWorkers = "SVP_OCR_RECOGNITION_PARALLEL_WORKERS";
    if (const std::optional<int> workers = positive_int(lookup, kWorkers)) {
      plan.ocr_recognition_workers = *workers;
      applied.push_back({.environment_variable = std::string(kWorkers),
                         .field = "ocr_recognition_workers",
                         .value = *workers});
    }
  }
  return applied;
}

}  // namespace svp::models
