#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/models/model_id.hpp"
#include "svp/vision/ocr_generation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace svp::vision::tasks {
namespace {

using Json = nlohmann::json;

// Graph optimization levels svp::models::OnnxSession maps to an ONNX Runtime
// level (0 disable, 1 basic, 2 extended, 3 layout, 99 all), plus -1 for "the
// runtime default", which is what every other value also means there.
constexpr std::array<int, 6> kGraphOptimizationLevels = {-1, 0, 1, 2, 3, 99};
constexpr int kRuntimeDefaultGraphOptimizationLevel = -1;

// ONNX Runtime execution modes OnnxSession understands; "" keeps the default.
constexpr std::array<std::string_view, 3> kExecutionModes = {"", "parallel",
                                                             "sequential"};
// Execution providers OnnxSession can create.
constexpr std::array<std::string_view, 2> kExecutionProviders = {"cpu", "coreml"};

[[noreturn]] void reject(const std::string& message) {
  throw std::invalid_argument("ocr.frame_batch parameters: " + message);
}

void require_fields(const Json& value, const std::set<std::string>& expected,
                    const std::string& where) {
  if (!value.is_object()) reject(where + " must be an object");
  for (const auto& [key, unused] : value.items()) {
    if (!expected.contains(key)) reject(where + " has unknown field `" + key + "`");
  }
  for (const std::string& key : expected) {
    if (!value.contains(key)) reject(where + " is missing field `" + key + "`");
  }
}

template <typename Integer>
Integer integer_at(const Json& object, const std::string& key, const std::string& where,
                   Integer minimum, Integer maximum = std::numeric_limits<Integer>::max()) {
  const Json& value = object.at(key);
  const std::string name = where + "." + key;
  if (!value.is_number_integer()) reject(name + " must be an integer");
  Integer parsed{};
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
      reject(name + " is out of range");
    }
    parsed = static_cast<Integer>(raw);
  } else {
    const auto raw = value.get<std::int64_t>();
    if constexpr (std::is_unsigned_v<Integer>) {
      reject(name + " must not be negative");
    } else {
      if (raw < static_cast<std::int64_t>(std::numeric_limits<Integer>::min()) ||
          raw > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
        reject(name + " is out of range");
      }
      parsed = static_cast<Integer>(raw);
    }
  }
  if (parsed < minimum || parsed > maximum) {
    reject(name + " must be in [" + std::to_string(minimum) + ", " +
           std::to_string(maximum) + "]");
  }
  return parsed;
}

double double_at(const Json& object, const std::string& key, const std::string& where,
                 double minimum, double maximum) {
  const Json& value = object.at(key);
  const std::string name = where + "." + key;
  // Encoders always write these as floating point, so an integer is
  // non-canonical.
  if (!value.is_number_float()) reject(name + " must be a floating-point number");
  const double parsed = value.get<double>();
  if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
    reject(name + " is out of range");
  }
  return parsed;
}

std::string string_at(const Json& object, const std::string& key, const std::string& where) {
  const Json& value = object.at(key);
  if (!value.is_string()) reject(where + "." + key + " must be a string");
  return value.get<std::string>();
}

template <std::size_t N>
std::string one_of(const Json& object, const std::string& key, const std::string& where,
                   const std::array<std::string_view, N>& allowed) {
  const std::string value = string_at(object, key, where);
  if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
    reject(where + "." + key + " `" + value + "` is not supported");
  }
  return value;
}

int graph_level_at(const Json& object, const std::string& where) {
  const int level = integer_at<int>(object, "graph_optimization_level", where,
                                    std::numeric_limits<int>::min());
  if (std::find(kGraphOptimizationLevels.begin(), kGraphOptimizationLevels.end(),
                level) == kGraphOptimizationLevels.end()) {
    reject(where + ".graph_optimization_level " + std::to_string(level) +
           " is not an ONNX Runtime level");
  }
  return level;
}

int normalized_graph_level(int level) {
  return std::find(kGraphOptimizationLevels.begin(), kGraphOptimizationLevels.end(),
                   level) == kGraphOptimizationLevels.end()
             ? kRuntimeDefaultGraphOptimizationLevel
             : level;
}

std::string model_id_at(const Json& object, const std::string& where) {
  const std::string id = string_at(object, "model_id", where);
  if (!svp::models::is_canonical_model_id(id)) {
    reject(where + ".model_id `" + id + "` is not a canonical model id");
  }
  return id;
}

// Thread counts must be explicit: kRuntimeChoosesThreadCount (0) would let
// each worker size its pools from its own host, and ONNX Runtime thread
// counts can change serialized output (plan §2.4 item 5).
svp::models::OrtThreadCounts threads_at(const Json& object, const std::string& where) {
  const std::string name = where + ".threads";
  const Json& value = object.at("threads");
  require_fields(value, {"inter_op", "intra_op"}, name);
  return svp::models::OrtThreadCounts{
      .intra_op = integer_at<int>(value, "intra_op", name, 1),
      .inter_op = integer_at<int>(value, "inter_op", name, 1),
  };
}

Json threads_to_json(const svp::models::OrtThreadCounts& threads) {
  return Json{{"inter_op", threads.inter_op}, {"intra_op", threads.intra_op}};
}

std::vector<OcrSample> samples_from_json(const Json& value) {
  const std::string where = "samples";
  require_fields(value, {"ordinals", "timestamps_us"}, where);
  const Json& ordinals = value.at("ordinals");
  const Json& timestamps = value.at("timestamps_us");
  if (!ordinals.is_array() || !timestamps.is_array()) {
    reject("samples.ordinals and samples.timestamps_us must be arrays");
  }
  if (ordinals.empty()) reject("samples must not be empty");
  if (ordinals.size() != timestamps.size()) {
    reject("samples.ordinals and samples.timestamps_us differ in length");
  }
  std::vector<OcrSample> samples;
  samples.reserve(ordinals.size());
  for (std::size_t index = 0; index < ordinals.size(); ++index) {
    const Json pair{{"ordinal", ordinals[index]}, {"timestamp_us", timestamps[index]}};
    const std::string item = where + "[" + std::to_string(index) + "]";
    OcrSample sample{
        .ordinal = integer_at<std::uint64_t>(pair, "ordinal", item, 0),
        .timestamp_us = integer_at<std::int64_t>(pair, "timestamp_us", item, 0),
    };
    if (!samples.empty() && (sample.ordinal <= samples.back().ordinal ||
                             sample.timestamp_us <= samples.back().timestamp_us)) {
      reject(item + " is not strictly after the sample before it");
    }
    samples.push_back(sample);
  }
  return samples;
}

}  // namespace

Json ocr_frame_batch_parameters_to_json(const OcrFrameBatchParameters& parameters) {
  const PpOcrOptions& ocr = parameters.pp_ocr;
  Json ordinals = Json::array();
  Json timestamps = Json::array();
  for (const OcrSample& sample : parameters.samples) {
    ordinals.push_back(sample.ordinal);
    timestamps.push_back(sample.timestamp_us);
  }
  Json value{
      {"decode", {{"ffmpeg_build", parameters.ffmpeg_build},
                  {"frame_height", parameters.frame_height},
                  {"frame_width", parameters.frame_width}}},
      {"detector",
       {{"box_thresh", ocr.det_box_thresh},
        {"execution_mode", ocr.det_execution_mode},
        {"graph_optimization_level",
         normalized_graph_level(ocr.det_graph_optimization_level)},
        {"limit_side_len", ocr.det_limit_side_len},
        {"model_id", ocr.detector_model_id},
        {"threads", threads_to_json(ocr.det_threads)},
        {"thresh", ocr.det_thresh},
        {"unclip_ratio", ocr.det_unclip_ratio}}},
      {"execution_provider", ocr.execution_provider},
      {"recognizer",
       {{"execution_mode", ocr.rec_execution_mode},
        {"graph_optimization_level",
         normalized_graph_level(ocr.rec_graph_optimization_level)},
        {"image_height", ocr.rec_image_height},
        {"max_width", ocr.rec_max_width},
        {"min_text_score", ocr.min_text_score},
        {"model_id", ocr.recognizer_model_id},
        {"parallel_min_boxes", ocr.recognition_parallel_min_boxes},
        {"parallel_workers", ocr.recognition_parallel_workers},
        {"threads", threads_to_json(ocr.rec_threads)}}},
      {"samples", {{"ordinals", std::move(ordinals)},
                   {"timestamps_us", std::move(timestamps)}}},
  };
  // One set of rules for both directions.
  (void)ocr_frame_batch_parameters_from_json(value);
  return value;
}

OcrFrameBatchParameters ocr_frame_batch_parameters_from_json(const Json& value) {
  require_fields(value,
                 {"decode", "detector", "execution_provider", "recognizer", "samples"},
                 "parameters");
  OcrFrameBatchParameters parameters;
  parameters.samples = samples_from_json(value.at("samples"));

  const Json& decode = value.at("decode");
  require_fields(decode, {"ffmpeg_build", "frame_height", "frame_width"}, "decode");
  parameters.ffmpeg_build = string_at(decode, "ffmpeg_build", "decode");
  if (!svp::exec::parse_blake3_prefixed(parameters.ffmpeg_build)) {
    reject("decode.ffmpeg_build must be b3:<64 hex>");
  }
  parameters.frame_width =
      integer_at<int>(decode, "frame_width", "decode", 1, kOcrMaxFrameDimension);
  parameters.frame_height =
      integer_at<int>(decode, "frame_height", "decode", 1, kOcrMaxFrameDimension);

  PpOcrOptions& ocr = parameters.pp_ocr;
  ocr.execution_provider =
      one_of(value, "execution_provider", "parameters", kExecutionProviders);

  const Json& det = value.at("detector");
  const std::string det_where = "detector";
  require_fields(det,
                 {"box_thresh", "execution_mode", "graph_optimization_level",
                  "limit_side_len", "model_id", "threads", "thresh", "unclip_ratio"},
                 det_where);
  ocr.detector_model_id = model_id_at(det, det_where);
  ocr.det_limit_side_len = integer_at<int>(det, "limit_side_len", det_where, 1);
  ocr.det_thresh = double_at(det, "thresh", det_where, 0.0, 1.0);
  ocr.det_box_thresh = double_at(det, "box_thresh", det_where, 0.0, 1.0);
  ocr.det_unclip_ratio = double_at(det, "unclip_ratio", det_where,
                                   std::numeric_limits<double>::min(),
                                   std::numeric_limits<double>::max());
  ocr.det_threads = threads_at(det, det_where);
  ocr.det_graph_optimization_level = graph_level_at(det, det_where);
  ocr.det_execution_mode = one_of(det, "execution_mode", det_where, kExecutionModes);

  const Json& rec = value.at("recognizer");
  const std::string rec_where = "recognizer";
  require_fields(rec,
                 {"execution_mode", "graph_optimization_level", "image_height",
                  "max_width", "min_text_score", "model_id", "parallel_min_boxes",
                  "parallel_workers", "threads"},
                 rec_where);
  ocr.recognizer_model_id = model_id_at(rec, rec_where);
  ocr.rec_image_height = integer_at<int>(rec, "image_height", rec_where, 1);
  ocr.rec_max_width = integer_at<int>(rec, "max_width", rec_where, 1);
  ocr.min_text_score = double_at(rec, "min_text_score", rec_where,
                                 std::numeric_limits<double>::lowest(),
                                 std::numeric_limits<double>::max());
  ocr.recognition_parallel_workers = integer_at<int>(rec, "parallel_workers", rec_where, 1);
  ocr.recognition_parallel_min_boxes =
      integer_at<int>(rec, "parallel_min_boxes", rec_where, 0);
  ocr.rec_threads = threads_at(rec, rec_where);
  ocr.rec_graph_optimization_level = graph_level_at(rec, rec_where);
  ocr.rec_execution_mode = one_of(rec, "execution_mode", rec_where, kExecutionModes);
  return parameters;
}

std::optional<std::string> validate_ocr_frame_batch_parameters(const Json& value) {
  try {
    (void)ocr_frame_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string("ocr.frame_batch parameters: ") + error.what();
  }
}

}  // namespace svp::vision::tasks
