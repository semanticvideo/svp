#include "svp/models/thread_plan.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace svp::models {
namespace {

// kRuntimeChoosesThreadCount is written as "auto" so a reader cannot mistake
// it for a real count of zero threads.
constexpr std::string_view kAuto = "auto";

nlohmann::json count_to_json(int value) {
  if (value == kRuntimeChoosesThreadCount) return std::string(kAuto);
  return value;
}

[[noreturn]] void fail(const std::string& path, const std::string& message) {
  throw std::invalid_argument("thread plan " + path + ": " + message);
}

const nlohmann::json& member(const nlohmann::json& object,
                             const std::string& key,
                             const std::string& path) {
  if (!object.is_object()) fail(path, "must be an object");
  const auto found = object.find(key);
  if (found == object.end()) fail(path + "." + key, "is required");
  return *found;
}

void require_exact_keys(const nlohmann::json& object,
                        std::initializer_list<std::string_view> keys,
                        const std::string& path) {
  if (!object.is_object()) fail(path, "must be an object");
  for (const auto& [key, value] : object.items()) {
    (void)value;
    bool known = false;
    for (const std::string_view allowed : keys) {
      known = known || key == allowed;
    }
    if (!known) fail(path + "." + key, "is not a thread plan field");
  }
}

int count_from_json(const nlohmann::json& object, const std::string& key,
                    const std::string& path) {
  const nlohmann::json& value = member(object, key, path);
  const std::string field = path + "." + key;
  if (value.is_string() && value.get<std::string>() == kAuto) {
    return kRuntimeChoosesThreadCount;
  }
  if (!value.is_number_integer()) fail(field, "must be an integer or \"auto\"");
  const auto parsed = value.get<std::int64_t>();
  if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
    fail(field, "must be a positive thread count or \"auto\"");
  }
  return static_cast<int>(parsed);
}

nlohmann::json ort_to_json(const OrtThreadCounts& counts) {
  return {{"intra_op", count_to_json(counts.intra_op)},
          {"inter_op", count_to_json(counts.inter_op)}};
}

OrtThreadCounts ort_from_json(const nlohmann::json& object,
                              const std::string& key,
                              const std::string& path) {
  const std::string role_path = path + "." + key;
  const nlohmann::json& role = member(object, key, path);
  require_exact_keys(role, {"intra_op", "inter_op"}, role_path);
  return {.intra_op = count_from_json(role, "intra_op", role_path),
          .inter_op = count_from_json(role, "inter_op", role_path)};
}

std::string_view source_name(ThreadPlanSource source) {
  switch (source) {
    case ThreadPlanSource::host:
      return "host";
    case ThreadPlanSource::supplied:
      return "supplied";
  }
  return "host";
}

}  // namespace

nlohmann::json thread_plan_to_json(const ThreadPlan& plan) {
  return {
      {"onnx_runtime",
       {{"ocr_detection", ort_to_json(plan.ocr_detection)},
        {"ocr_recognition", ort_to_json(plan.ocr_recognition)},
        {"depth", ort_to_json(plan.depth)},
        {"visual_entity_detection", ort_to_json(plan.visual_entity_detection)},
        {"visual_entity_embedding", ort_to_json(plan.visual_entity_embedding)},
        {"text_embedding", ort_to_json(plan.text_embedding)},
        {"speech_activity", ort_to_json(plan.speech_activity)},
        {"forced_alignment", ort_to_json(plan.forced_alignment)}}},
      {"ocr_recognition_workers", count_to_json(plan.ocr_recognition_workers)},
      {"whisper",
       {{"decode_threads", count_to_json(plan.whisper.decode)},
        {"vad_threads", count_to_json(plan.whisper.vad)}}},
      {"sherpa",
       {{"segmentation_threads", count_to_json(plan.sherpa.segmentation)},
        {"embedding_threads", count_to_json(plan.sherpa.embedding)}}},
      {"opencv_threads", count_to_json(plan.opencv)},
  };
}

ThreadPlan thread_plan_from_json(const nlohmann::json& value) {
  const std::string root = "$";
  require_exact_keys(value,
                     {"onnx_runtime", "ocr_recognition_workers", "whisper",
                      "sherpa", "opencv_threads"},
                     root);
  const std::string ort_path = root + ".onnx_runtime";
  const nlohmann::json& ort = member(value, "onnx_runtime", root);
  require_exact_keys(ort,
                     {"ocr_detection", "ocr_recognition", "depth",
                      "visual_entity_detection", "visual_entity_embedding",
                      "text_embedding", "speech_activity", "forced_alignment"},
                     ort_path);
  ThreadPlan plan;
  plan.ocr_detection = ort_from_json(ort, "ocr_detection", ort_path);
  plan.ocr_recognition = ort_from_json(ort, "ocr_recognition", ort_path);
  plan.depth = ort_from_json(ort, "depth", ort_path);
  plan.visual_entity_detection =
      ort_from_json(ort, "visual_entity_detection", ort_path);
  plan.visual_entity_embedding =
      ort_from_json(ort, "visual_entity_embedding", ort_path);
  plan.text_embedding = ort_from_json(ort, "text_embedding", ort_path);
  plan.speech_activity = ort_from_json(ort, "speech_activity", ort_path);
  plan.forced_alignment = ort_from_json(ort, "forced_alignment", ort_path);
  plan.ocr_recognition_workers =
      count_from_json(value, "ocr_recognition_workers", root);

  const std::string whisper_path = root + ".whisper";
  const nlohmann::json& whisper = member(value, "whisper", root);
  require_exact_keys(whisper, {"decode_threads", "vad_threads"}, whisper_path);
  plan.whisper.decode = count_from_json(whisper, "decode_threads", whisper_path);
  plan.whisper.vad = count_from_json(whisper, "vad_threads", whisper_path);

  const std::string sherpa_path = root + ".sherpa";
  const nlohmann::json& sherpa = member(value, "sherpa", root);
  require_exact_keys(sherpa, {"segmentation_threads", "embedding_threads"},
                     sherpa_path);
  plan.sherpa.segmentation =
      count_from_json(sherpa, "segmentation_threads", sherpa_path);
  plan.sherpa.embedding =
      count_from_json(sherpa, "embedding_threads", sherpa_path);

  plan.opencv = count_from_json(value, "opencv_threads", root);
  require_valid_thread_plan(plan);
  return plan;
}

nlohmann::json thread_plan_resolution_to_json(
    const ThreadPlanResolution& resolution) {
  nlohmann::json overrides = nlohmann::json::array();
  for (const ThreadPlanOverride& applied : resolution.overrides) {
    overrides.push_back({{"environment_variable", applied.environment_variable},
                         {"field", applied.field},
                         {"value", applied.value}});
  }
  return {
      {"source", std::string(source_name(resolution.source))},
      {"host", {{"logical_cpus", resolution.host.logical_cpus}}},
      {"plan", thread_plan_to_json(resolution.plan)},
      {"host_independent", thread_plan_is_host_independent(resolution.plan)},
      {"environment_overrides", std::move(overrides)},
  };
}

}  // namespace svp::models
