#pragma once

// The PP-OCR part of a task's parameters, shared by every task type that
// runs PP-OCR (ocr.frame_batch, ocr.crop_batch): every value that can change
// output, thread counts included (plan §2.4 item 5), and never the
// worker-local model cache root or manifest filename.
//
//   "detector":{"box_thresh","execution_mode","graph_optimization_level",
//               "limit_side_len","model_id","threads":{"inter_op","intra_op"},
//               "thresh","unclip_ratio"},
//   "execution_provider",
//   "recognizer":{"execution_mode","graph_optimization_level","image_height",
//                 "max_width","min_text_score","model_id",
//                 "parallel_min_boxes","parallel_workers",
//                 "threads":{"inter_op","intra_op"}}
//
// A graph optimization level ONNX Runtime does not name is sent as -1, the
// runtime default it already behaves as.

#include "svp/vision/pp_ocr.hpp"

#include <nlohmann/json.hpp>

#include <string_view>

namespace svp::vision::tasks {

// The three PP-OCR fields above, to merge into a parameters object.
[[nodiscard]] nlohmann::json pp_ocr_parameter_fields(const PpOcrOptions& options);

// Reads the three fields from `parameters` (other fields are the caller's).
// Throws std::invalid_argument "<task_type> parameters: <reason>" for a
// missing, unknown, mistyped, or out-of-range value.
[[nodiscard]] PpOcrOptions pp_ocr_options_from_parameters(const nlohmann::json& parameters,
                                                          std::string_view task_type);

}  // namespace svp::vision::tasks
