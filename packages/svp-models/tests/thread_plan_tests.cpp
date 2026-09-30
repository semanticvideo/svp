#include "svp/models/thread_plan.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using svp::models::HostCpuTopology;
using svp::models::kRuntimeChoosesThreadCount;
using svp::models::OrtThreadCounts;
using svp::models::ThreadPlan;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

svp::models::EnvironmentLookup lookup_from(
    std::map<std::string, std::string> values) {
  return [values = std::move(values)](
             std::string_view name) -> std::optional<std::string> {
    const auto found = values.find(std::string(name));
    if (found == values.end()) return std::nullopt;
    return found->second;
  };
}

std::vector<OrtThreadCounts> ort_roles(const ThreadPlan& plan) {
  return {plan.ocr_detection,       plan.ocr_recognition,
          plan.depth,               plan.visual_entity_detection,
          plan.visual_entity_embedding, plan.text_embedding,
          plan.speech_activity,     plan.forced_alignment};
}

// The ONNX Runtime value must follow the host, not a fixed core count.
void test_onnx_runtime_default_follows_host() {
#if defined(__APPLE__)
  for (unsigned logical : {0U, 1U, 2U, 3U, 8U, 10U, 12U, 14U, 16U, 24U, 32U}) {
    const int expected = std::max(1, static_cast<int>(logical / 2));
    expect(svp::models::onnx_runtime_default_pool_threads({logical}) == expected,
           "ORT default pool is max(1, logical_cpus / 2) on Apple");
  }
#else
  expect(svp::models::onnx_runtime_default_pool_threads({16}) ==
             kRuntimeChoosesThreadCount,
         "ORT default is left to ONNX Runtime off Apple");
#endif
}

void test_local_plan_reproduces_previous_defaults() {
  for (unsigned logical : {1U, 4U, 8U, 10U, 16U, 24U}) {
    const ThreadPlan plan = svp::models::resolve_local_thread_plan({logical}, 3);
    const int ort = svp::models::onnx_runtime_default_pool_threads({logical});
    for (const OrtThreadCounts& role : ort_roles(plan)) {
      expect(role.intra_op == ort && role.inter_op == ort,
             "every ORT role uses the host ORT default");
    }
    expect(plan.ocr_recognition_workers == 3, "OCR workers come from the caller");
    // Previous whisper backend rule: min(hardware_concurrency, 8), at least 1.
    expect(plan.whisper.decode ==
               static_cast<int>(std::min(std::max(logical, 1U), 8U)),
           "whisper decode threads keep the previous host rule");
    expect(plan.whisper.vad == 4, "whisper VAD keeps whisper.cpp's default");
    expect(plan.sherpa.segmentation == 1 && plan.sherpa.embedding == 1,
           "sherpa stays single-threaded");
    expect(plan.opencv == kRuntimeChoosesThreadCount,
           "OpenCV is recorded as runtime-chosen");
    expect(svp::models::thread_plan_problems(plan).empty(),
           "resolved local plan is valid");
  }
  const ThreadPlan unknown = svp::models::resolve_local_thread_plan({0}, 2);
  expect(unknown.whisper.decode == 1,
         "unknown host core count falls back to one whisper thread");
}

void test_detect_host_matches_hardware_concurrency() {
  expect(svp::models::detect_host_cpu_topology().logical_cpus ==
             std::thread::hardware_concurrency(),
         "host topology reads hardware_concurrency");
}

void test_validation() {
  ThreadPlan plan = svp::models::resolve_local_thread_plan({10}, 2);
  plan.whisper.decode = 0;
  plan.ocr_recognition_workers = 0;
  plan.depth.intra_op = -1;
  const auto problems = svp::models::thread_plan_problems(plan);
  expect(problems.size() == 3, "three problems reported");
  bool threw = false;
  try {
    svp::models::require_valid_thread_plan(plan);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  expect(threw, "invalid plan throws");
}

void test_host_independence() {
  ThreadPlan plan = svp::models::resolve_local_thread_plan({10}, 2);
#if defined(__APPLE__)
  expect(svp::models::thread_plan_is_host_independent(plan),
         "Apple local plan pins every configured runtime");
#endif
  plan.speech_activity.inter_op = kRuntimeChoosesThreadCount;
  expect(!svp::models::thread_plan_is_host_independent(plan),
         "an auto ORT role makes the plan host-dependent");
}

void test_environment_overrides() {
  const ThreadPlan base = svp::models::resolve_local_thread_plan({10}, 2);

  ThreadPlan unchanged = base;
  const auto none = svp::models::apply_thread_plan_environment_overrides(
      unchanged, lookup_from({{"SVP_OCR_ONNX_INTRA_OP_THREADS", "0"},
                              {"SVP_OCR_REC_ONNX_INTRA_OP_THREADS", "x"},
                              {"SVP_OCR_ONNX_INTER_OP_THREADS", ""}}),
      true);
  expect(none.empty() && unchanged == base,
         "non-positive or unparsable values do not override");

  ThreadPlan shared = base;
  const auto shared_applied =
      svp::models::apply_thread_plan_environment_overrides(
          shared, lookup_from({{"SVP_OCR_ONNX_INTRA_OP_THREADS", "7"}}), false);
  expect(shared.ocr_detection.intra_op == 7 &&
             shared.ocr_recognition.intra_op == 7,
         "shared OCR variable sets detector and recognizer");
  expect(shared.ocr_detection.inter_op == base.ocr_detection.inter_op,
         "inter-op untouched");
  expect(shared.depth == base.depth, "non-OCR roles untouched");
  expect(shared_applied.size() == 2 &&
             shared_applied[0].environment_variable ==
                 "SVP_OCR_ONNX_INTRA_OP_THREADS" &&
             shared_applied[0].field == "onnx_runtime.ocr_detection.intra_op" &&
             shared_applied[0].value == 7,
         "shared override is recorded per field");

  ThreadPlan role = base;
  const auto role_applied = svp::models::apply_thread_plan_environment_overrides(
      role,
      lookup_from({{"SVP_OCR_ONNX_INTRA_OP_THREADS", "7"},
                   {"SVP_OCR_REC_ONNX_INTRA_OP_THREADS", "2"},
                   {"SVP_OCR_DET_ONNX_INTER_OP_THREADS", "3"}}),
      false);
  expect(role.ocr_recognition.intra_op == 2 && role.ocr_detection.intra_op == 7,
         "role-specific variable wins over the shared one");
  expect(role.ocr_detection.inter_op == 3 &&
             role.ocr_recognition.inter_op == base.ocr_recognition.inter_op,
         "role-specific inter-op applies to its role only");
  expect(role_applied.size() == 3, "three overrides recorded");

  ThreadPlan workers = base;
  const auto gated = svp::models::apply_thread_plan_environment_overrides(
      workers, lookup_from({{"SVP_OCR_RECOGNITION_PARALLEL_WORKERS", "6"}}),
      false);
  expect(gated.empty() && workers.ocr_recognition_workers == 2,
         "worker override needs diagnostics enabled");
  const auto ungated = svp::models::apply_thread_plan_environment_overrides(
      workers, lookup_from({{"SVP_OCR_RECOGNITION_PARALLEL_WORKERS", "6"}}),
      true);
  expect(ungated.size() == 1 && workers.ocr_recognition_workers == 6,
         "worker override applies with diagnostics enabled");
}

void test_json_round_trip() {
  ThreadPlan plan = svp::models::resolve_local_thread_plan({12}, 6);
  plan.depth.inter_op = kRuntimeChoosesThreadCount;
  const nlohmann::json json = svp::models::thread_plan_to_json(plan);
  expect(json.at("opencv_threads") == "auto", "auto is written as \"auto\"");
  expect(json.at("onnx_runtime").at("depth").at("inter_op") == "auto",
         "auto ORT count written as \"auto\"");
  expect(svp::models::thread_plan_from_json(json) == plan, "round trip");

  const auto rejects = [](nlohmann::json value, std::string_view message) {
    bool threw = false;
    try {
      (void)svp::models::thread_plan_from_json(value);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    expect(threw, message);
  };
  nlohmann::json zero = json;
  zero["whisper"]["decode_threads"] = 0;
  rejects(zero, "zero count rejected");
  nlohmann::json auto_workers = json;
  auto_workers["ocr_recognition_workers"] = "auto";
  rejects(auto_workers, "auto workers rejected");
  nlohmann::json extra = json;
  extra["cuda_threads"] = 2;
  rejects(extra, "unknown field rejected");
  nlohmann::json missing = json;
  missing["onnx_runtime"].erase("forced_alignment");
  rejects(missing, "missing role rejected");
}

void test_resolution_json() {
  svp::models::ThreadPlanResolution resolution;
  resolution.plan = svp::models::resolve_local_thread_plan({10}, 2);
  resolution.source = svp::models::ThreadPlanSource::supplied;
  resolution.host = {10};
  resolution.overrides.push_back(
      {"SVP_OCR_ONNX_INTRA_OP_THREADS", "onnx_runtime.ocr_detection.intra_op", 4});
  const nlohmann::json json =
      svp::models::thread_plan_resolution_to_json(resolution);
  expect(json.at("source") == "supplied", "source recorded");
  expect(json.at("host").at("logical_cpus") == 10, "host recorded");
  expect(json.at("environment_overrides").size() == 1, "override recorded");
  expect(json.at("plan") == svp::models::thread_plan_to_json(resolution.plan),
         "plan recorded");
}

}  // namespace

int main() {
  test_onnx_runtime_default_follows_host();
  test_local_plan_reproduces_previous_defaults();
  test_detect_host_matches_hardware_concurrency();
  test_validation();
  test_host_independence();
  test_environment_overrides();
  test_json_round_trip();
  test_resolution_json();
  if (failures != 0) {
    std::cerr << failures << " thread plan test(s) failed\n";
    return 1;
  }
  std::cout << "thread plan tests passed\n";
  return 0;
}
