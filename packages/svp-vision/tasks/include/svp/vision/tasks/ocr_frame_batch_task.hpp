#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/tasks/pp_ocr_session_pool.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace svp::vision::tasks {

// What a runtime provides to ocr.frame_batch beyond the TaskSpec: its own
// model cache and ffmpeg (never sent by the coordinator, plan §4.3: no
// message carries a path), and the store its outputs go to.
struct OcrFrameBatchWorkerEnvironment {
  // The model cache PP-OCR loads from, unless model_cache_for is set.
  std::filesystem::path model_cache_root;
  // The ffmpeg this runtime decodes with. Its ffmpeg_build_identity() must
  // equal the spec's decode.ffmpeg_build.
  std::filesystem::path ffmpeg_path;
  // Stores output bytes in the runtime's TaskArtifactAccess store (for
  // example CasTaskArtifactAccess::put) and returns their ref.
  std::function<svp::exec::ArtifactRef(std::span<const std::byte> bytes,
                                       std::string media_type, std::string role)>
      write_output;
  // Optional: the model cache for one spec, for runtimes that hold several
  // verified bundles per model (a worker's content-addressed model store:
  // the cache is a view over exactly the bundles the spec's model_refs name).
  // Throwing means this runtime cannot provide those bundles now.
  std::function<std::filesystem::path(const svp::exec::TaskSpec& spec)> model_cache_for;
  // True only on the coordinator: a batch whose OCR cannot start (ffmpeg
  // cannot decode at all, PP-OCR does not load) succeeds with every sample
  // not_started, so the OCR stage falls back to running exactly as a build
  // without batches does. False (workers): a retryable failure, so the batch
  // runs on another Mac and output never depends on which Mac took it.
  bool record_start_failures = false;
};

// Registers ocr.frame_batch version 1 with its strict parameter validator.
//
// The task decodes the spec's samples from its `source` input and runs PP-OCR
// on each, returning one OcrSampleDetections JSONL output
// (kOcrFrameDetectionsRole), one record per sample in parameter order. Before
// running it requires the spec's model_refs to name exactly the parameters'
// detector and recognizer, and the worker's loaded bundles to match those
// refs' bundle_blake3. PP-OCR sessions are pooled for the registry's lifetime
// (see PpOcrSessionPool). Cancellation is checked between frames.
//
// Failures that another worker may not share are retryable failed results:
// ocr_unavailable (PP-OCR could not load here), model_unavailable (this
// runtime cannot provide the named bundles), model_mismatch (this worker's
// bundles differ from the refs), decode_unavailable (no usable ffmpeg here),
// decoder_mismatch (this runtime's ffmpeg is another build than the spec's).
// A spec whose model_refs disagree with its parameters is invalid_model_refs,
// permanent. Per-frame decode misses and PP-OCR errors are data in the
// payload, as they are for a local build.
//
// `sessions` is the pool tasks take PP-OCR sessions from; null gives the
// registration a pool of its own. A coordinator passes the pool its OCR
// reducer also uses.
void register_ocr_frame_batch_task(svp::exec::TaskTypeRegistry& registry,
                                   OcrFrameBatchWorkerEnvironment environment,
                                   std::shared_ptr<PpOcrSessionPool> sessions = nullptr);

}  // namespace svp::vision::tasks
