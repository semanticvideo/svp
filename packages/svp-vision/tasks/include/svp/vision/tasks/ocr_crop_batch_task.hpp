#pragma once

#include "svp/exec/task_registry.hpp"
#include "svp/vision/tasks/dispatched_task_environment.hpp"
#include "svp/vision/tasks/pp_ocr_session_pool.hpp"

#include <memory>

namespace svp::vision::tasks {

// Registers ocr.crop_batch version 1 with its strict parameter validator.
//
// The task runs run_evidence_crop_job for each job, in parameter order, on its
// `source` input: ffmpeg cuts the crop, then the crop is decoded back and
// re-read with PP-OCR recognition. It returns one record per job
// (kOcrCropRecordsRole: ordinal, extracted, and when extracted the image's
// byte range, roi_decoded, roi_text, roi_score) and the images back to back
// (kOcrCropImagesRole). A ROI score that is not finite is reported as
// roi_decoded false, so the stage re-reads that crop itself. PP-OCR sessions
// come from `sessions` (null: a pool of the registration's own; the
// coordinator passes the pool its OCR stage uses). Cancellation is checked
// between jobs.
//
// Failures another worker may not share are retryable: ocr_unavailable,
// model_unavailable, model_mismatch, decode_unavailable, decoder_mismatch
// (as for ocr.frame_batch). A spec whose inputs or model_refs disagree with
// its parameters is permanent. A job ffmpeg cannot extract is data
// (extracted false); the stage extracts it again itself.
void register_ocr_crop_batch_task(svp::exec::TaskTypeRegistry& registry,
                                  DispatchedTaskEnvironment environment,
                                  std::shared_ptr<PpOcrSessionPool> sessions = nullptr);

}  // namespace svp::vision::tasks
