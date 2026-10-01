#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/ocr_batch_policy.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Coordinator side of ocr.frame_batch: building the TaskSpec for one batch of
// the OCR sample plan, and reading its output back for the reducer
// (svp::vision::reduce_ocr_frame_batches).

// Scheduling lane of OCR frame batches; batches are ordered by first ordinal.
inline constexpr std::string_view kOcrFrameBatchLane = "ocr.frame_batch";

// Peak RSS of one worker running PP-OCRv6 medium det + rec on ONNX Runtime
// CPU: plan §2.3 measured 1.2-1.5 GB; the top of that range is used so
// admission never under-reserves.
inline constexpr std::uint64_t kOcrFrameBatchEstimatedPeakRssMb = 1500;

struct OcrFrameBatchTaskInputs {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  // The source media, role kOcrFrameBatchSourceRole.
  svp::exec::ArtifactRef source;
  // From ocr_frame_batch_model_refs().
  std::vector<svp::exec::TaskModelRef> model_refs;
  // From make_ocr_pp_ocr_options(): explicit thread counts required.
  PpOcrOptions pp_ocr;
  // Its per-sample estimate sizes the task's est_seconds.
  OcrBatchPolicy batch_policy;
};

// TaskModelRefs for the detector and recognizer named by `pp_ocr`, read from
// their bundle manifests under pp_ocr.model_cache_root. Throws
// std::runtime_error when a manifest is missing or malformed.
[[nodiscard]] std::vector<svp::exec::TaskModelRef> ocr_frame_batch_model_refs(
    const PpOcrOptions& pp_ocr);

// "task.ocr.frame_batch.samples_<first>_<last>" with six-digit (minimum)
// ordinals, last inclusive.
[[nodiscard]] std::string ocr_frame_batch_task_id(const OcrSampleBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey ocr_frame_batch_order_key(
    const OcrSampleBatch& batch);

// The validated TaskSpec for `batch` of `plan`. cache_key is
// compute_cache_key(["ocr.frame_batch", version, source blake3,
// [model_bundle_id...], parameters_blake3]): the source bytes, model bundles,
// and every output-affecting parameter (thread counts and the samples
// included) identify the result. Throws std::invalid_argument for a batch
// outside the plan or parameters that fail validation.
[[nodiscard]] svp::exec::TaskSpec make_ocr_frame_batch_task_spec(
    const OcrFrameBatchTaskInputs& inputs,
    const OcrSamplePlan& plan,
    const OcrSampleBatch& batch);

// Decodes a committed ocr.frame_batch output and checks it is one record per
// spec sample, in spec order, with the spec's timestamps (plan §4.4 unit
// count). Throws OcrFrameDetectionsCodecError for bad bytes and
// std::invalid_argument for records that do not match the spec.
[[nodiscard]] std::vector<OcrSampleDetections> read_ocr_frame_batch_output(
    const svp::exec::TaskSpec& spec,
    std::string_view payload);

}  // namespace svp::vision::tasks
