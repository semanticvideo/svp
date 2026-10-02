#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/evidence_crop_work.hpp"
#include "svp/vision/pp_ocr.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Coordinator side of ocr.crop_batch: the TaskSpec for one batch of the
// evidence-crop jobs, and reading its output back for the stage.

inline constexpr std::string_view kOcrCropBatchLane = "ocr.crop_batch";

// Peak RSS of one task (admission, plan §3.5): a worker process holding the
// PP-OCRv6 detector and recognizer and re-reading 1080p crops peaked at
// 214 MiB (svp-vision-dispatched-work-tests, SVP_DISPATCH_TEST_PEAK_RSS). The
// crop is cut and decoded by ffmpeg processes of their own, and the
// recognizer's input is bounded by PpOcrOptions::rec_max_width whatever the
// crop's size, so the estimate leaves about 80% above the measurement.
inline constexpr std::uint64_t kOcrCropBatchEstimatedPeakRssMb = 384;

struct OcrCropBatchTaskInputs {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  // The source media, role kOcrFrameBatchSourceRole.
  svp::exec::ArtifactRef source;
  // The PP-OCR detector and recognizer (ocr_frame_batch_model_refs).
  std::vector<svp::exec::TaskModelRef> model_refs;
  // The OCR stage's PP-OCR options: explicit thread counts required.
  PpOcrOptions pp_ocr;
  std::string ffmpeg_build;
  // Its per-item estimate sizes the task's est_seconds.
  ItemBatchPolicy batch_policy;
};

// The validated TaskSpec for jobs [batch.first, batch.first + batch.count).
// Throws std::invalid_argument for a batch outside `jobs`.
[[nodiscard]] svp::exec::TaskSpec make_ocr_crop_batch_task_spec(
    const OcrCropBatchTaskInputs& inputs, const std::vector<EvidenceCropJob>& jobs,
    const ItemBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey ocr_crop_batch_order_key(const ItemBatch& batch);

// The parameters bytes one job adds to a spec, for batch sizing.
[[nodiscard]] std::uint64_t ocr_crop_job_parameter_bytes(const EvidenceCropJob& job);

// Decodes a committed ocr.crop_batch result: one outcome per spec job, in
// spec order (plan §4.4 unit count). Throws std::invalid_argument for
// outputs that do not match the spec.
[[nodiscard]] std::vector<EvidenceCropJobOutcome> read_ocr_crop_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads);

}  // namespace svp::vision::tasks
