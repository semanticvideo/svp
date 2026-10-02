#pragma once

// The OCR stage as frame-batch tasks (plan §3.1, §4.2, M3): the coordinator
// plans the OCR sample schedule once, cuts it into ocr.frame_batch tasks that
// may run in this process or on paired workers, and the `ocr` stage task
// becomes the reducer over their committed results.
//
//   ocr deps ─┬─ task.ocr.frame_batch.samples_000000_000015 ─┐
//             ├─ task.ocr.frame_batch.samples_000016_000031 ─┼─ task.ocr.vstream_000
//             └─ ...                                         ┘   (reduce, crops)
//
// Batches depend on exactly what the whole OCR stage depended on, so lane
// order and concurrency are unchanged; the reducer depends on every batch.
// Output is the same bytes whatever the partition or completion order
// (svp::vision::OcrBatchPolicy, reduce_ocr_frame_batches).
//
// The stage only splits when every batch can run the batched path the stage
// itself would run: a package build, the model runtime available, a media
// plan with a decode size and at least one sample, and an ffmpeg whose build
// identity can be read (that identity is what every executor must decode
// with). Otherwise the `ocr` task runs the whole stage in-process as before.

#include "engine/stage_task_plan.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/ocr_batch_policy.hpp"
#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::engine {

// Everything a frame-batch task carries besides its slice of the plan: the
// sample plan, PP-OCR options (thread counts explicit, this Mac's model
// cache), the model bundles they load, the decoder identity, and the source.
struct OcrWorkPlan {
  svp::vision::OcrSamplePlan samples;
  svp::vision::PpOcrOptions pp_ocr;
  std::vector<svp::exec::TaskModelRef> model_refs;
  std::string ffmpeg_build;
  svp::exec::ArtifactRef source;
  std::filesystem::path source_path;
};

struct OcrWorkPlanInputs {
  const BuildPipelineOptions& options;
  const BuildStageExecutionPlan& stage_plan;
  const svp::media::MediaIngestPlan& media_plan;
  const nlohmann::json& media_plan_json;
  const svp::models::ThreadPlan& thread_plan;
  bool model_runtime_available = false;
  // The source's whole-file BLAKE3 and size (the journal's fingerprint).
  svp::exec::Blake3Digest source_blake3{};
  std::uint64_t source_bytes = 0;
};

// nullopt when the stage does not split (see above).
[[nodiscard]] std::optional<OcrWorkPlan> plan_ocr_work(const OcrWorkPlanInputs& inputs);

// The batch tasks of one build, in batch (canonical) order.
struct OcrFrameBatchPlan {
  OcrWorkPlan work;
  std::vector<svp::vision::OcrSampleBatch> batches;
  std::vector<svp::exec::TaskNode> nodes;
};

// Cuts `work` into batches by `policy` and builds their TaskSpecs; each batch
// depends on `depends_on` (the OCR stage task's own dependencies).
[[nodiscard]] OcrFrameBatchPlan make_ocr_frame_batch_plan(
    OcrWorkPlan work, const svp::vision::OcrBatchPolicy& policy,
    const std::string& build_session_id, const std::vector<std::string>& depends_on);

// The whole-stage graph of make_stage_task_graph with `batches` spliced in:
// the batch nodes are added, and the `ocr` stage task also depends on every
// one of them.
[[nodiscard]] svp::exec::TaskGraph make_build_task_graph(
    const std::vector<PlannedStageTask>& tasks, const std::string& build_session_id,
    const std::string& build_inputs_blake3, const OcrFrameBatchPlan* batches);

// The OCR stage task's dependencies, as task IDs.
[[nodiscard]] std::vector<std::string> ocr_stage_dependencies(
    const std::vector<PlannedStageTask>& tasks);

// Batch partition recorded in an existing journal's task IDs, for a resumed
// build: the batches must be exactly the ones whose results were committed,
// whatever the per-sample estimate is now. nullopt when `task_ids` hold no
// frame-batch task or they do not cut [0, sample_count) into consecutive
// batches.
[[nodiscard]] std::optional<std::vector<svp::vision::OcrSampleBatch>>
ocr_batches_from_task_ids(const std::vector<std::string>& task_ids, std::size_t sample_count);

// make_ocr_frame_batch_plan with an explicit partition.
[[nodiscard]] OcrFrameBatchPlan make_ocr_frame_batch_plan_from_batches(
    OcrWorkPlan work, std::vector<svp::vision::OcrSampleBatch> batches,
    const svp::vision::OcrBatchPolicy& policy, const std::string& build_session_id,
    const std::vector<std::string>& depends_on);

}  // namespace svp::builder::engine
