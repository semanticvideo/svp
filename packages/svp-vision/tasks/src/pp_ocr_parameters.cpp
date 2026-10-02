#include "svp/vision/tasks/pp_ocr_parameters.hpp"

#include "parameter_fields.hpp"

#include <array>
#include <limits>

namespace svp::vision::tasks {
namespace {

using detail::Json;
using detail::ParameterFields;

// Graph optimization levels svp::models::OnnxSession maps to an ONNX Runtime
// level (0 disable, 1 basic, 2 extended, 3 layout, 99 all), plus -1 for "the
// runtime default", which is what every other value also means there.
constexpr std::array<int, 6> kGraphOptimizationLevels = {-1, 0, 1, 2, 3, 99};
constexpr int kRuntimeDefaultGraphOptimizationLevel = -1;

// ONNX Runtime execution modes OnnxSession understands; "" keeps the default.
constexpr std::array<std::string_view, 3> kExecutionModes = {"", "parallel",
                                                             "sequential"};

int graph_level_at(const ParameterFields& fields, const Json& object,
                   const std::string& where) {
  const int level = fields.integer_at<int>(object, "graph_optimization_level", where,
                                           std::numeric_limits<int>::min());
  if (std::find(kGraphOptimizationLevels.begin(), kGraphOptimizationLevels.end(),
                level) == kGraphOptimizationLevels.end()) {
    fields.reject(where + ".graph_optimization_level " + std::to_string(level) +
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

}  // namespace

Json pp_ocr_parameter_fields(const PpOcrOptions& ocr) {
  return Json{
      {"detector",
       {{"box_thresh", ocr.det_box_thresh},
        {"execution_mode", ocr.det_execution_mode},
        {"graph_optimization_level",
         normalized_graph_level(ocr.det_graph_optimization_level)},
        {"limit_side_len", ocr.det_limit_side_len},
        {"model_id", ocr.detector_model_id},
        {"threads", detail::threads_to_json(ocr.det_threads)},
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
        {"threads", detail::threads_to_json(ocr.rec_threads)}}},
  };
}

PpOcrOptions pp_ocr_options_from_parameters(const Json& value, std::string_view task_type) {
  const ParameterFields fields(task_type);
  PpOcrOptions ocr;
  ocr.execution_provider =
      fields.one_of(value, "execution_provider", "parameters", detail::kExecutionProviders);

  const Json& det = value.at("detector");
  const std::string det_where = "detector";
  fields.require_fields(det,
                        {"box_thresh", "execution_mode", "graph_optimization_level",
                         "limit_side_len", "model_id", "threads", "thresh", "unclip_ratio"},
                        det_where);
  ocr.detector_model_id = fields.model_id_at(det, det_where);
  ocr.det_limit_side_len = fields.integer_at<int>(det, "limit_side_len", det_where, 1);
  ocr.det_thresh = fields.double_at(det, "thresh", det_where, 0.0, 1.0);
  ocr.det_box_thresh = fields.double_at(det, "box_thresh", det_where, 0.0, 1.0);
  ocr.det_unclip_ratio = fields.double_at(det, "unclip_ratio", det_where,
                                          std::numeric_limits<double>::min(),
                                          std::numeric_limits<double>::max());
  ocr.det_threads = fields.threads_at(det, det_where);
  ocr.det_graph_optimization_level = graph_level_at(fields, det, det_where);
  ocr.det_execution_mode = fields.one_of(det, "execution_mode", det_where, kExecutionModes);

  const Json& rec = value.at("recognizer");
  const std::string rec_where = "recognizer";
  fields.require_fields(rec,
                        {"execution_mode", "graph_optimization_level", "image_height",
                         "max_width", "min_text_score", "model_id", "parallel_min_boxes",
                         "parallel_workers", "threads"},
                        rec_where);
  ocr.recognizer_model_id = fields.model_id_at(rec, rec_where);
  ocr.rec_image_height = fields.integer_at<int>(rec, "image_height", rec_where, 1);
  ocr.rec_max_width = fields.integer_at<int>(rec, "max_width", rec_where, 1);
  ocr.min_text_score = fields.double_at(rec, "min_text_score", rec_where,
                                        std::numeric_limits<double>::lowest(),
                                        std::numeric_limits<double>::max());
  ocr.recognition_parallel_workers =
      fields.integer_at<int>(rec, "parallel_workers", rec_where, 1);
  ocr.recognition_parallel_min_boxes =
      fields.integer_at<int>(rec, "parallel_min_boxes", rec_where, 0);
  ocr.rec_threads = fields.threads_at(rec, rec_where);
  ocr.rec_graph_optimization_level = graph_level_at(fields, rec, rec_where);
  ocr.rec_execution_mode = fields.one_of(rec, "execution_mode", rec_where, kExecutionModes);
  return ocr;
}

}  // namespace svp::vision::tasks
