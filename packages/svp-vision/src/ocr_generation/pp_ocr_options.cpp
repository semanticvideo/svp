#include "svp/vision/ocr_generation.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <cstdlib>
#include <string>

namespace svp::vision {
namespace {

int positive_env_int_or_default(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed <= 0) return fallback;
  return static_cast<int>(parsed);
}

int graph_opt_env_or_default(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  if (std::string(value) == "disable") return 0;
  if (std::string(value) == "basic") return 1;
  if (std::string(value) == "extended") return 2;
  if (std::string(value) == "layout") return 3;
  if (std::string(value) == "all") return 99;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) return fallback;
  return static_cast<int>(parsed);
}

std::string execution_mode_env_or_default(const char* name,
                                          const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  const std::string parsed(value);
  if (parsed == "parallel" || parsed == "sequential") return parsed;
  return fallback;
}

std::string execution_provider_env_or_default(const char* name,
                                              const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  const std::string parsed(value);
  if (parsed == "cpu" || parsed == "coreml") return parsed;
  return fallback;
}

}  // namespace

PpOcrOptions make_ocr_pp_ocr_options(const OcrGenerationOptions& options) {
  PpOcrOptions pp_ocr_opts;
  pp_ocr_opts.model_cache_root = options.model_cache_root;
  pp_ocr_opts.recognition_parallel_workers = options.recognition_parallel_workers;
  pp_ocr_opts.recognition_parallel_min_boxes =
      options.recognition_parallel_min_boxes;
  pp_ocr_opts.execution_provider = execution_provider_env_or_default(
      "SVP_OCR_EXECUTION_PROVIDER", pp_ocr_opts.execution_provider);
  pp_ocr_opts.graph_optimization_level = graph_opt_env_or_default(
      "SVP_OCR_ONNX_GRAPH_OPT_LEVEL", pp_ocr_opts.graph_optimization_level);
  pp_ocr_opts.execution_mode = execution_mode_env_or_default(
      "SVP_OCR_ONNX_EXECUTION_MODE", pp_ocr_opts.execution_mode);
  pp_ocr_opts.det_threads = options.detection_threads;
  pp_ocr_opts.det_graph_optimization_level = graph_opt_env_or_default(
      "SVP_OCR_DET_ONNX_GRAPH_OPT_LEVEL", pp_ocr_opts.graph_optimization_level);
  pp_ocr_opts.det_execution_mode = execution_mode_env_or_default(
      "SVP_OCR_DET_ONNX_EXECUTION_MODE", pp_ocr_opts.execution_mode);
  pp_ocr_opts.rec_threads = options.recognition_threads;
  pp_ocr_opts.rec_graph_optimization_level = graph_opt_env_or_default(
      "SVP_OCR_REC_ONNX_GRAPH_OPT_LEVEL", pp_ocr_opts.graph_optimization_level);
  pp_ocr_opts.rec_execution_mode = execution_mode_env_or_default(
      "SVP_OCR_REC_ONNX_EXECUTION_MODE", pp_ocr_opts.execution_mode);
  if (svp::core::memory_diagnostics_enabled()) {
    pp_ocr_opts.recognition_parallel_min_boxes = positive_env_int_or_default(
        "SVP_OCR_RECOGNITION_PARALLEL_MIN_BOXES",
        pp_ocr_opts.recognition_parallel_min_boxes);
  }

  return pp_ocr_opts;
}

}  // namespace svp::vision
